/**
 * Day 10 tests: CodeExecTool, GrepTool, GitTool.
 */

#include "tool/code_exec_tool.h"
#include "tool/git_tool.h"
#include "tool/grep_tool.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cpp_agent;
namespace fs = std::filesystem;

// ============================================================================
// Setup: temp directory with test files
// ============================================================================
class TestEnv {
public:
    TestEnv() {
        root_ = fs::temp_directory_path() / "cpp-agent-test-d10";
        fs::remove_all(root_);
        fs::create_directories(root_);

        write_file("main.cpp", "int main() {\n    return 0;\n}\n");
        write_file("utils.h",   "#pragma once\nvoid foo();\n");
        write_file("utils.cpp", "#include \"utils.h\"\nvoid foo() { return; }\n");
        write_file("data.txt",  "TODO: refactor this\nFIXME: memory leak\nregular line\n");
    }
    ~TestEnv() { fs::remove_all(root_); }

    const fs::path& root() const { return root_; }

private:
    void write_file(const std::string& name, const std::string& content) {
        std::ofstream f(root_ / name);
        f << content;
    }
    fs::path root_;
};

// ============================================================================
// CodeExecTool
// ============================================================================
static bool test_code_exec_python() {
    CodeExecTool tool(std::chrono::seconds(5));

    json args = {{"language", "python"}, {"code", "print(1+1)"}};
    auto result = tool.execute(args, "call_1");

    assert(result.success);
    assert(result.content.find("2") != std::string::npos);
    return true;
}

static bool test_code_exec_shell() {
    CodeExecTool tool(std::chrono::seconds(5));

    json args = {{"language", "shell"}, {"code", "echo hello_from_shell"}};
    auto result = tool.execute(args, "call_2");

    assert(result.success);
    assert(result.content.find("hello_from_shell") != std::string::npos);
    return true;
}

static bool test_code_exec_timeout() {
    // Use a 1-second timeout for a command that should take longer
    CodeExecTool tool(std::chrono::seconds(1));

    // Python infinite loop — should be killed after 1s
    json args = {{"language", "python"},
                 {"code", "import time; time.sleep(10); print('never')"}};
    auto started = std::chrono::steady_clock::now();
    auto result = tool.execute(args, "call_3");

    // The result should mention timeout
    assert(result.content.find("timeout") != std::string::npos);
    assert(!result.success);
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    return true;
}

static bool test_code_exec_error() {
    CodeExecTool tool(std::chrono::seconds(5));

    json args = {{"language", "python"}, {"code", "1/0"}};
    auto result = tool.execute(args, "call_4");

    // Python prints ZeroDivisionError traceback to stderr
    // which is redirected to stdout via 2>&1
    assert(!result.content.empty());
    assert(!result.success);
    return true;
}

// ============================================================================
// GrepTool
// ============================================================================
static bool test_grep_basic() {
    TestEnv env;
    GrepTool tool(env.root());

    json args = {{"pattern", "int main"}, {"path", "."}};
    auto result = tool.execute(args, "call_5");

    assert(result.success);
    assert(result.content.find("main.cpp") != std::string::npos);
    assert(result.content.find("int main") != std::string::npos);
    return true;
}

static bool test_grep_with_glob() {
    TestEnv env;
    GrepTool tool(env.root());

    json args = {{"pattern", "void"}, {"path", "."}, {"glob", "*.cpp"}};
    auto result = tool.execute(args, "call_6");

    assert(result.success);
    // Should find void in utils.cpp
    assert(result.content.find("void") != std::string::npos);
    return true;
}

static bool test_grep_todo() {
    TestEnv env;
    GrepTool tool(env.root());

    json args = {{"pattern", "TODO|FIXME"}, {"path", "."}};
    auto result = tool.execute(args, "call_7");

    assert(result.success);
    assert(result.content.find("TODO") != std::string::npos);
    assert(result.content.find("FIXME") != std::string::npos);
    assert(result.content.find("data.txt") != std::string::npos);
    return true;
}

static bool test_grep_not_found() {
    TestEnv env;
    GrepTool tool(env.root());

    json args = {{"pattern", "XYZZY_NONEXISTENT"}, {"path", "."}};
    auto result = tool.execute(args, "call_8");

    assert(result.success);
    assert(result.content.find("0 match") != std::string::npos);
    return true;
}

static bool test_grep_invalid_regex() {
    TestEnv env;
    GrepTool tool(env.root());

    json args = {{"pattern", "[invalid(regex"}, {"path", "."}};
    auto result = tool.execute(args, "call_9");

    assert(!result.success);
    assert(result.content.find("invalid regex") != std::string::npos);
    return true;
}

// ============================================================================
// GitTool
// ============================================================================
static bool test_git_status() {
    // Run on the actual project repo (should have uncommitted changes or be clean)
    fs::path project_root = fs::current_path();
    // Navigate up from build/ to project root
    while (!fs::exists(project_root / "CMakeLists.txt") && project_root.has_parent_path()) {
        project_root = project_root.parent_path();
    }

    GitTool tool(project_root);
    json args = {{"action", "status"}};
    auto result = tool.execute(args, "call_10");

    assert(result.success);
    // Status should return something (even if clean)
    assert(!result.content.empty());
    std::cout << "  git status: " << result.content.size() << " chars\n";
    return true;
}

static bool test_git_log() {
    fs::path project_root = fs::current_path();
    while (!fs::exists(project_root / "CMakeLists.txt") && project_root.has_parent_path()) {
        project_root = project_root.parent_path();
    }

    GitTool tool(project_root);
    json args = {{"action", "log"}, {"count", 3}};
    auto result = tool.execute(args, "call_11");

    assert(result.success);
    // Might say "no output" if repo is not a git repo — that's fine
    std::cout << "  git log: " << result.content.substr(0, 80) << "...\n";
    return true;
}

static bool test_git_unknown_action() {
    GitTool tool(fs::current_path());
    json args = {{"action", "rebase_all"}};
    auto result = tool.execute(args, "call_12");

    assert(!result.success);
    assert(result.content.find("unknown action") != std::string::npos);
    return true;
}

// ============================================================================
// Runner
// ============================================================================
int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {
        // CodeExecTool
        {"code exec: python 1+1", test_code_exec_python},
        {"code exec: shell echo", test_code_exec_shell},
        {"code exec: timeout kill", test_code_exec_timeout},
        {"code exec: error output", test_code_exec_error},
        // GrepTool
        {"grep: basic search", test_grep_basic},
        {"grep: glob filter", test_grep_with_glob},
        {"grep: TODO/FIXME", test_grep_todo},
        {"grep: no matches", test_grep_not_found},
        {"grep: invalid regex", test_grep_invalid_regex},
        // GitTool
        {"git: status", test_git_status},
        {"git: log", test_git_log},
        {"git: unknown action", test_git_unknown_action},
    };

    int passed = 0, failed = 0;
    for (auto& [name, fn] : tests) {
        std::cout << "[TEST] " << name << " ... ";
        try {
            if (fn()) {
                std::cout << "PASSED\n";
                ++passed;
            } else {
                std::cout << "FAILED\n";
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cout << "FAILED (" << e.what() << ")\n";
            ++failed;
        }
    }

    std::cout << "\n=== " << passed << "/" << (passed + failed) << " passed ===\n";
    return failed > 0 ? 1 : 0;
}
