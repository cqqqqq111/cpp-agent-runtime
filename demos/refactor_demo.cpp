/**
 * @file refactor_demo.cpp
 * @brief Day 21 demo #3 — "refactor" scenario.
 *
 * The scripted agent plans a rename refactor: it greps for every reference to
 * a symbol, confirms the shape of one usage, and returns a step-by-step
 * migration plan. Demonstrates search_content → read_file → plan loop.
 *
 * Run from the project root (path-sensitive):
 *   ./build/demos/refactor_demo
 */

#include "demos/demo_common.h"

#include "tool/fs_tool.h"
#include "tool/grep_tool.h"

int main() {
    using namespace demo;

    std::vector<ScriptedResponse> script = {
        // Round 1: locate every reference to the target symbol.
        {true, "", {
            make_call("call_1", "grep",
                      R"({"pattern":"last_run_iterations","path":"src","glob":"*.h"})"),
            make_call("call_2", "grep",
                      R"({"pattern":"last_run_iterations","path":"src","glob":"*.cpp"})"),
        }},
        // Round 2: inspect one declaration site to confirm the shape.
        {true, "", {
            make_call("call_3", "filesystem",
                      R"({"action":"read_file","path":"src/agent/react_loop.h"})"),
        }},
        // Round 3: migration plan (no more tool calls).
        {false,
         "Refactor plan: rename ReActLoop::last_run_iterations → iteration_count\n"
         "  1. Declaration (react_loop.h): change the getter name.\n"
         "  2. Uses in react_loop.cpp: update the two return points.\n"
         "  3. Call sites in demos/demo_common.h: update run_demo()'s summary print.\n"
         "  4. Rebuild + run ctest to confirm no regressions.\n"
         "Impact: mechanical rename, 3 files, no behavior change.", {}, "stop"},
    };

    return run_demo(
        "refactor",
        "Plan a rename of ReActLoop::last_run_iterations to iteration_count.",
        "You are a careful refactoring assistant that never edits code, only plans.",
        std::move(script),
        [](cpp_agent::ToolRegistry& reg) {
            reg.register_tool(std::make_unique<cpp_agent::GrepTool>());
            reg.register_tool(std::make_unique<cpp_agent::FileSystemTool>());
        });
}
