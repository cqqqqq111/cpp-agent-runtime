#include "tool/process.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace cpp_agent {
std::string ProcessResult::describe() const {
    std::string text = output;
    if (truncated) text += "\n(output truncated)";
    if (timed_out) text += "\n(command timeout; process terminated)";
    else if (exit_code != 0) text += "\n(process exited with code " + std::to_string(exit_code) + ")";
    return text.empty() ? "(no output)" : text;
}

namespace {
void append_output(ProcessResult& result, const char* data, size_t length, size_t limit) {
    size_t keep = std::min(length, limit - result.output.size());
    result.output.append(data, keep);
    result.truncated |= keep != length;
}

#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring wide(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!n && !text.empty()) throw std::runtime_error("Invalid UTF-8 process argument");
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), n);
    return result;
}
std::wstring quote(const std::string& arg) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : wide(arg)) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
#else
struct Fd {
    int value = -1;
    ~Fd() { if (value >= 0) close(value); }
};
struct Child {
    pid_t pid;
    bool reaped = false;
    ~Child() {
        // Also stop descendants that outlived the immediate child.
        kill(-pid, SIGKILL);
        if (!reaped) while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
};
#endif
}

ProcessResult run_process(const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout, size_t max_bytes) {
    if (args.empty() || args[0].empty() || timeout.count() <= 0)
        throw std::invalid_argument("Process requires an executable and a positive timeout");
    for (const auto& arg : args)
        if (arg.find('\0') != std::string::npos) throw std::invalid_argument("NUL in process argument");
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> buffer{};
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe.value, &write_pipe.value, &sa, 0) ||
        !SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Cannot create process pipe");
    Handle null_input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, 0, nullptr)};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("Cannot create process job");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input.value;
    startup.hStdOutput = startup.hStdError = write_pipe.value;
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quote(arg); }
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info))
        throw std::runtime_error("Cannot start process");
    Handle process{info.hProcess}, thread{info.hThread};
    if (!AssignProcessToJobObject(job.value, process.value)) {
        TerminateProcess(process.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        throw std::runtime_error("Cannot assign child to process job");
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(process.value, INFINITE);
        throw std::runtime_error("Cannot resume child process");
    }
    CloseHandle(write_pipe.value); write_pipe.value = nullptr;
    while (true) {
        DWORD available = 0;
        bool pipe_open = PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr, &available, nullptr) != 0;
        if (pipe_open && available) {
            DWORD count = 0;
            if (ReadFile(read_pipe.value, buffer.data(), std::min<DWORD>(available, buffer.size()), &count, nullptr))
                append_output(result, buffer.data(), count, max_bytes);
        }
        bool exited = WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0;
        if (exited && (!pipe_open || available == 0)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, INFINITE);
            break;
        }
        if (!available) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    DWORD code = 1;
    GetExitCodeProcess(process.value, &code);
    result.exit_code = static_cast<int>(code);
#else
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) != 0) throw std::runtime_error("Cannot create process pipe");
    Fd read_pipe{pipes[0]}, write_pipe{pipes[1]};
    if (fcntl(read_pipe.value, F_SETFL, O_NONBLOCK) < 0)
        throw std::runtime_error("Cannot set pipe nonblocking");
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&actions);
    posix_spawnattr_init(&attributes);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, write_pipe.value, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, write_pipe.value, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, read_pipe.value);
    posix_spawn_file_actions_addclose(&actions, write_pipe.value);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    int error = posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error != 0) throw std::runtime_error("Cannot start process: " + std::to_string(error));
    Child child{pid};
    close(write_pipe.value); write_pipe.value = -1;
    bool eof = false;
    int status = 0;
    while (true) {
        // Bound each drain pass so a continuously writing child cannot starve the deadline.
        for (int batch = 0; batch < 16 && !eof; ++batch) {
            ssize_t count = read(read_pipe.value, buffer.data(), buffer.size());
            if (count > 0) append_output(result, buffer.data(), static_cast<size_t>(count), max_bytes);
            else if (count == 0) eof = true;
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else throw std::runtime_error("Cannot read child output");
        }
        if (!child.reaped) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) child.reaped = true;
            else if (waited < 0 && errno != EINTR) throw std::runtime_error("Cannot reap child");
        }
        if (child.reaped && eof) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            if (!child.reaped) {
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                child.reaped = true;
            }
            break;
        }
        pollfd fd{read_pipe.value, POLLIN, 0};
        if (!eof) poll(&fd, 1, 5);
        else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
    return result;
}
} // namespace cpp_agent
