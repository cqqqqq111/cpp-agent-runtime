/**
 * @file debug_demo.cpp
 * @brief Day 21 demo #2 — "debug" scenario.
 *
 * The scripted agent triages a reported bug: it greps the codebase for a
 * suspicious pattern, opens the file that matches, and proposes a root cause
 * plus a fix. Demonstrates the grep → read_file → diagnose loop.
 *
 * Run from the project root (path-sensitive):
 *   ./build/demos/debug_demo
 */

#include "demos/demo_common.h"

#include "tool/fs_tool.h"
#include "tool/grep_tool.h"

int main() {
    using namespace demo;

    std::vector<ScriptedResponse> script = {
        // Round 1: find where the suspect construct is used.
        {true, "", {
            make_call("call_1", "grep",
                      R"({"pattern":"wait_for","path":"src","glob":"*.cpp"})"),
        }},
        // Round 2: read the file that surfaced.
        {true, "", {
            make_call("call_2", "filesystem",
                      R"({"action":"read_file","path":"src/agent/react_loop.cpp"})"),
        }},
        // Round 3: root cause + fix (no more tool calls).
        {false,
         "Root cause: the per-iteration timeout calls future.wait_for() then "
         "worker.detach() on timeout, but a detached worker can outlive the "
         "lambda's captured copies. Diagnosis:\n"
         "  • Captures are by-value (msgs_copy, tools_copy), so no dangling refs.\n"
         "  • Fix: on the timeout path we return early WITHOUT touching `resp`, "
         "and the detached thread owns all its data — this is safe.\n"
         "Suggested hardening: cap the number of concurrent timed-out workers.", {}, "stop"},
    };

    return run_demo(
        "debug",
        "The agent hangs occasionally on a stuck LLM call. Find the timeout code and diagnose.",
        "You are a systems engineer debugging a race condition.",
        std::move(script),
        [](cpp_agent::ToolRegistry& reg) {
            reg.register_tool(std::make_unique<cpp_agent::GrepTool>());
            reg.register_tool(std::make_unique<cpp_agent::FileSystemTool>());
        });
}
