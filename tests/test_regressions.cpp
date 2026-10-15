#include "agent/react_loop.h"
#include "memory/vector_index.h"
#include "tool/code_exec_tool.h"
#include "tool/fs_tool.h"
#include "tool/git_tool.h"
#include "tool/grep_tool.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

using namespace cpp_agent;
namespace fs = std::filesystem;

struct Fixture {
    fs::path root = fs::temp_directory_path() / ("cpp-agent-regression-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directories(root / "allowed"); }
    ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};

void test_process_lifecycle() {
    CodeExecTool exec(std::chrono::seconds(1), 128);
    auto result = exec.execute({{"language", "shell"}, {"code", "exit 7"}}, "exit");
    assert(!result.success && result.content.find("code 7") != std::string::npos);
    result = exec.execute({{"language", "python"}, {"code", "print('quotes: \\\" $HOME `literal`')"}}, "quotes");
    assert(result.success && result.content.find("$HOME `literal`") != std::string::npos);
    result = exec.execute({{"language", "python"}, {"code", "print('x' * 1000000)"}}, "limit");
    assert(result.success && result.content.size() < 200 && result.content.find("truncated") != std::string::npos);
    auto started = std::chrono::steady_clock::now();
    result = exec.execute({{"language", "python"}, {"code", "while True: print('x' * 4096)"}}, "flood");
    assert(!result.success && result.content.find("timeout") != std::string::npos);
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));

    Fixture fixture;
    auto marker = fixture.root / "descendant.txt";
    std::string child = "import time,pathlib; time.sleep(1.8); pathlib.Path(" +
        json(marker.string()).dump() + ").write_text('survived')";
    std::string parent = "import subprocess,sys,time; subprocess.Popen([sys.executable,'-c'," +
        json(child).dump() + "]); time.sleep(10)";
    result = exec.execute({{"language", "python"}, {"code", parent}}, "tree");
    assert(!result.success);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    assert(!fs::exists(marker));
}

void test_path_boundaries() {
    Fixture fixture;
    const auto allowed = fixture.root / "allowed";
    std::ofstream(fixture.root / "outside.txt") << "OUTSIDE_MARKER\n";
    std::ofstream(allowed / "inside.txt") << "INSIDE_MARKER\n";
    GrepTool grep(allowed);
    for (const auto& path : {std::string("../outside.txt"), (fixture.root / "outside.txt").string()}) {
        auto result = grep.execute({{"path", path}, {"pattern", "OUTSIDE_MARKER"}}, "escape");
        assert(!result.success);
    }
    auto result = grep.execute({{"path", "."}, {"pattern", "INSIDE_MARKER"}}, "inside");
    assert(result.success && result.content.find("inside.txt:") != std::string::npos);
    std::error_code error;
    fs::create_symlink(fixture.root / "outside.txt", allowed / "linked.txt", error);
#ifndef _WIN32
    assert(!error);
#endif
    if (!error) {
        result = grep.execute({{"path", "."}, {"pattern", "OUTSIDE_MARKER"}}, "symlink");
        assert(result.success && result.content.find("0 match") != std::string::npos);
        FileSystemTool tool(allowed);
        result = tool.execute({{"action", "search_content"}, {"path", "."}, {"pattern", "OUTSIDE_MARKER"}}, "symlink");
        assert(result.success && result.content.find("0 match") != std::string::npos);
        result = tool.execute({{"action", "read_file"}, {"path", "linked.txt"}}, "direct");
        assert(!result.success);
    }
    GitTool git(fixture.root / "not a repo ' quoted");
    result = git.execute({{"action", "status"}}, "git");
    assert(!result.success);
}

void test_message_groups() {
    MessageHistory history;
    history.push(Message::user("test"));
    history.push(Message::assistant_tool_calls({{"a", "echo", "{}"}, {"b", "echo", "{}"}}));
    history.push(Message::tool_result("a", "echo", "ok"));
    history.push(Message::tool_result("b", "echo", "ok"));
    history.push(Message::assistant("final"));
    history.truncate_to(3);
    assert(history.size() == 1 && history.messages().front().content == "final");
    assert(history.estimated_tokens() == 1);
}

void test_vector_validation() {
    VectorIndex index;
    index.add({1, 0}, "keep");
    auto rejects = [](auto action) {
        bool threw = false;
        try { action(); } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    };
    rejects([&] { index.add({1}, "bad"); });
    rejects([&] { index.search({1}); });
    rejects([&] { index.search({std::numeric_limits<float>::quiet_NaN(), 0}); });
    Fixture fixture;
    auto file = fixture.root / "bad.json";
    std::ofstream(file) << R"({"dim":2,"entries":[{"vector":[1],"metadata":"bad"}]})";
    rejects([&] { index.load(file.string()); });
    assert(index.size() == 1 && index.search({1, 0})[0].metadata == "keep");
}

int main() {
    test_process_lifecycle();
    test_path_boundaries();
    test_message_groups();
    test_vector_validation();
    std::cout << "Process lifecycle, path boundaries, message groups and vector validation passed\n";
}
