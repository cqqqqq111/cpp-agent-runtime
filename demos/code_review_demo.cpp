/**
 * @file code_review_demo.cpp
 * @brief Day 21 demo #1 — "code review" scenario.
 *
 * A scripted agent acts as a code reviewer: it first reads the file under
 * review, then greps the tree for TODO/FIXME markers, and finally produces a
 * review verdict. Real tools, real files, canned LLM — deterministic and
 * fully offline, so the resulting logs/code_review.json trace is reproducible.
 *
 * Run from the project root (path-sensitive):
 *   ./build/demos/code_review_demo
 */

#include "demos/demo_common.h"

#include "tool/fs_tool.h"
#include "tool/grep_tool.h"

int main() {
    using namespace demo;

    std::vector<ScriptedResponse> script = {
        // Round 1: read the file under review.
        {true, "", {
            make_call("call_1", "filesystem",
                      R"({"action":"read_file","path":"src/tool/fs_tool.h"})"),
        }},
        // Round 2: scan the tree for leftover markers.
        {true, "", {
            make_call("call_2", "grep",
                      R"({"pattern":"TODO|FIXME|XXX","path":"src","glob":"*.cpp"})"),
        }},
        // Round 3: final review verdict (no more tool calls).
        {false,
         "Review of src/tool/fs_tool.h:\n"
         "  • Path traversal is correctly blocked via canonicalization against "
         "allowed_root_ (validate_path). Good.\n"
         "  • Three actions (read_file / list_dir / search_content) are cleanly "
         "split into private helpers.\n"
         "  • No TODO/FIXME markers remain in src/*.cpp.\n"
         "Verdict: APPROVE — no blocking issues.", {}, "stop"},
    };

    return run_demo(
        "code_review",
        "Review src/tool/fs_tool.h for security and style issues.",
        "You are a meticulous senior code reviewer.",
        std::move(script),
        [](cpp_agent::ToolRegistry& reg) {
            reg.register_tool(std::make_unique<cpp_agent::FileSystemTool>());
            reg.register_tool(std::make_unique<cpp_agent::GrepTool>());
        });
}
