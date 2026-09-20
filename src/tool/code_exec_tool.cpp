#include "tool/code_exec_tool.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace cpp_agent {

// ============================================================================
// Helper: run a command with timeout via popen
// ============================================================================
static std::string popen_read(const std::string& cmd,
                               std::chrono::seconds timeout,
                               size_t max_bytes) {
    // RAII wrapper for FILE*
    struct PipeCloser { void operator()(FILE* f) const { if (f) _pclose(f); } };
    using Pipe = std::unique_ptr<FILE, PipeCloser>;

    Pipe pipe = Pipe(_popen(cmd.c_str(), "r"));
    if (!pipe) {
        throw std::runtime_error("popen failed for: " + cmd);
    }

    // Run the read in an async task so we can enforce timeout
    auto read_task = std::async(std::launch::async, [&]() {
        std::string output;
        std::array<char, 4096> buf{};
        while (output.size() < max_bytes && std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) {
            output += buf.data();
        }
        return output;
    });

    auto status = read_task.wait_for(timeout);
    if (status == std::future_status::timeout) {
        // Kill the child by closing the pipe; the read task will finish.
        pipe.reset();  // closes the pipe, child gets SIGPIPE / broken pipe
        spdlog::warn("CodeExecTool: command timed out after {}s", timeout.count());
        return "(timeout after " + std::to_string(timeout.count()) + "s)";
    }

    std::string output = read_task.get();
    if (output.size() >= max_bytes) {
        output += "\n(output truncated at " + std::to_string(max_bytes) + " bytes)";
    }
    if (output.empty()) {
        output = "(no output)";
    }
    return output;
}

// ============================================================================
// CodeExecTool
// ============================================================================

CodeExecTool::CodeExecTool(std::chrono::seconds timeout, size_t max_output_bytes)
    : timeout_(timeout), max_output_bytes_(max_output_bytes) {}

json CodeExecTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"language", {
                {"type", "string"},
                {"enum", {"python", "shell"}},
                {"description", "Language to execute: 'python' or 'shell'."}
            }},
            {"code", {
                {"type", "string"},
                {"description", "The source code or shell command to execute."}
            }}
        }},
        {"required", json::array({"language", "code"})}
    };
}

ToolResult CodeExecTool::execute(const json& args, const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    std::string language = args.value("language", "");
    std::string code = args.value("code", "");

    if (code.empty()) {
        tr.content = "Error: 'code' parameter is empty.";
        tr.success = false;
        return tr;
    }

    std::string cmd;
    if (language == "python") {
        // Use -c to execute inline code
        // Escape double quotes in the code for the shell
        std::string escaped = code;
        for (size_t pos = 0; (pos = escaped.find('"', pos)) != std::string::npos; pos += 2) {
            escaped.insert(pos, "\\");
        }
        cmd = "python -c \"" + escaped + "\" 2>&1";
    } else if (language == "shell") {
        cmd = code + " 2>&1";
    } else {
        tr.content = "Error: unknown language '" + language +
                     "'. Use 'python' or 'shell'.";
        tr.success = false;
        return tr;
    }

    spdlog::info("CodeExecTool: executing {} ({} chars)", language, code.size());

    try {
        tr.content = popen_read(cmd, timeout_, max_output_bytes_);
        tr.success = true;
    } catch (const std::exception& e) {
        tr.content = std::string("Execution error: ") + e.what();
        tr.success = false;
    }

    return tr;
}

}  // namespace cpp_agent
