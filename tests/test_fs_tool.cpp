/**
 * Day 5 tests: FileSystemTool — read, list, search + path security.
 */

#include "tool/fs_tool.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace cpp_agent;
namespace fs = std::filesystem;

// ============================================================================
// Setup: create a temporary test directory
// ============================================================================
class TestDir {
public:
    TestDir() {
        root_ = fs::temp_directory_path() / "cpp-agent-test-d5";
        fs::remove_all(root_);
        fs::create_directories(root_);
        fs::create_directories(root_ / "sub");

        // Create test files
        write_file(root_ / "hello.txt", "Hello, world!\nThis is a test file.\nLine three.");
        write_file(root_ / "sub" / "nested.txt", "Nested content in subdirectory.\nMore content.");
        write_file(root_ / "code.cpp", "int main() {\n    return 1+1;\n}\n// End.");
    }

    ~TestDir() { fs::remove_all(root_); }

    const fs::path& root() const { return root_; }

private:
    void write_file(const fs::path& p, const std::string& content) {
        std::ofstream f(p);
        f << content;
    }

    fs::path root_;
};

// ============================================================================
// Test 1: read_file — normal case
// ============================================================================
static bool test_read_file() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "read_file"}, {"path", "hello.txt"}};
    auto result = tool.execute(args, "call_1");

    assert(result.success);
    assert(result.content.find("Hello, world!") != std::string::npos);
    assert(result.content.find("│") != std::string::npos);  // line numbers

    return true;
}

// ============================================================================
// Test 2: read_file — nonexistent
// ============================================================================
static bool test_read_file_not_found() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "read_file"}, {"path", "nonexistent.txt"}};
    auto result = tool.execute(args, "call_2");

    assert(!result.success);
    assert(result.content.find("not found") != std::string::npos);

    return true;
}

// ============================================================================
// Test 3: list_dir
// ============================================================================
static bool test_list_dir() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "list_dir"}, {"path", "."}};
    auto result = tool.execute(args, "call_3");

    assert(result.success);
    // Should find all our test files
    assert(result.content.find("hello.txt") != std::string::npos);
    assert(result.content.find("sub") != std::string::npos);
    assert(result.content.find("code.cpp") != std::string::npos);

    return true;
}

// ============================================================================
// Test 4: search_content
// ============================================================================
static bool test_search_content() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "search_content"}, {"path", "."}, {"pattern", "test"}};
    auto result = tool.execute(args, "call_4");

    assert(result.success);
    assert(result.content.find("hello.txt") != std::string::npos);
    assert(result.content.find("test") != std::string::npos);

    return true;
}

// ============================================================================
// Test 5: search_content with regex
// ============================================================================
static bool test_search_regex() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "search_content"}, {"path", "."}, {"pattern", R"(int\s+main)"}};
    auto result = tool.execute(args, "call_5");

    assert(result.success);
    assert(result.content.find("code.cpp") != std::string::npos);
    assert(result.content.find("int main") != std::string::npos);

    return true;
}

// ============================================================================
// Test 6: PATH TRAVERSAL ATTACK — blocked!
// ============================================================================
static bool test_path_traversal_blocked() {
    TestDir td;
    FileSystemTool tool(td.root());

    // Attempt to escape the sandbox with ".."
    json args = {{"action", "read_file"}, {"path", "../../../etc/passwd"}};
    auto result = tool.execute(args, "call_6");

    assert(!result.success);
    assert(result.content.find("outside") != std::string::npos);

    // Also try the Windows variant
    args = {{"action", "list_dir"}, {"path", "..\\..\\..\\Windows"}};
    result = tool.execute(args, "call_7");

    assert(!result.success);
    assert(result.content.find("outside") != std::string::npos);

    return true;
}

// ============================================================================
// Test 7: Absolute path within allowed root
// ============================================================================
static bool test_absolute_path_within_root() {
    TestDir td;
    FileSystemTool tool(td.root());

    // Use the absolute path of a file inside the sandbox
    fs::path abs_path = td.root() / "hello.txt";
    json args = {{"action", "read_file"}, {"path", abs_path.string()}};
    auto result = tool.execute(args, "call_8");

    assert(result.success);
    assert(result.content.find("Hello, world!") != std::string::npos);

    return true;
}

// ============================================================================
// Test 8: Unknown action
// ============================================================================
static bool test_unknown_action() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "delete_all_files"}, {"path", "."}};
    auto result = tool.execute(args, "call_9");

    assert(!result.success);
    assert(result.content.find("unknown action") != std::string::npos);

    return true;
}

// ============================================================================
// Test 9: search without pattern
// ============================================================================
static bool test_search_no_pattern() {
    TestDir td;
    FileSystemTool tool(td.root());

    json args = {{"action", "search_content"}, {"path", "."}};
    auto result = tool.execute(args, "call_10");

    assert(!result.success);
    assert(result.content.find("pattern") != std::string::npos);

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
        {"read_file normal", test_read_file},
        {"read_file not found", test_read_file_not_found},
        {"list_dir", test_list_dir},
        {"search_content", test_search_content},
        {"search_content regex", test_search_regex},
        {"PATH TRAVERSAL BLOCKED", test_path_traversal_blocked},
        {"absolute path within root", test_absolute_path_within_root},
        {"unknown action rejected", test_unknown_action},
        {"search without pattern → error", test_search_no_pattern},
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
