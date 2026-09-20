/**
 * @file real_agent_demo.cpp
 * @brief One-shot REAL agent demo — real LLM (DeepSeek via .env) + real tools.
 *
 * Unlike code_review_demo / debug_demo / refactor_demo (which use a canned
 * ScriptedLLM), this one drives the ReAct loop with the actual LLM configured
 * in .env (DeepSeek / OpenAI-compatible). It runs a single query, records the
 * full reasoning trace to logs/real_agent.json, and prints the answer.
 *
 * Usage (run from project root — path-sensitive):
 *   ./build/demos/real_agent_demo                # default query
 *   ./build/demos/real_agent_demo "你的问题"
 *
 * Needs the Python runtime + mingw64 on PATH (same as ./build/cpp-agent).
 */

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "llm/llm_adapter.h"
#include "llm/py_bridge.h"
#include "tool/fs_tool.h"
#include "tool/git_tool.h"
#include "tool/grep_tool.h"
#include "tool/tool_base.h"
#include "tracing/trace_recorder.h"

#include <fmt/core.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

using namespace cpp_agent;

int main(int argc, char* argv[]) {
    std::string query = argc > 1
        ? argv[1]
        : "帮我检查一下这个代码库：用工具找出 src 目录里所有的 TODO/FIXME 注释，"
          "然后告诉我它们分别出现在哪些文件、大致是什么内容。";

    if (init_python_bridge() != 0) {
        fmt::print(stderr, "Python bridge init failed.\n");
        return 1;
    }

    // Ask Python which model is configured (reads .env on the Python side).
    std::string model = "deepseek-chat";
    try {
        json cfg = json::parse(llm_config_bridge());
        model = cfg.value("model", model);
    } catch (const std::exception& e) {
        fmt::print(stderr, "config check failed: {}\n", e.what());
    }

    ToolRegistry registry;
    registry.register_tool(std::make_unique<FileSystemTool>());
    registry.register_tool(std::make_unique<GrepTool>());
    registry.register_tool(std::make_unique<GitTool>());

    AgentConfig cfg;
    cfg.system_prompt =
        "You are a coding assistant inspecting a C++ repository. "
        "Use the grep/filesystem/git tools to look at real code, then answer "
        "concisely in Chinese. Prefer grep for finding patterns and "
        "filesystem read_file for reading a specific file.";
    cfg.max_iterations = 6;
    // NOTE: leave iteration_timeout_ms = 0 here. The Python bridge holds the
    // GIL on the main thread (pybind11 scoped_interpreter), so moving the LLM
    // call onto a timeout worker std::thread deadlocks on GIL acquisition.
    // Timeout is enforced at the HTTP layer instead (see python/llm_client.py).
    cfg.iteration_timeout_ms = 0;

    auto llm = make_python_llm_adapter(model);
    fmt::print("LLM backend: {}\n\n", llm->name());

    ReActLoop agent(*llm, registry, cfg);

    TraceRecorder recorder("logs/real_agent.json");
    recorder.set_config(cfg);
    recorder.set_query(query);
    agent.set_trace_callback(recorder.wrap({}));

    auto t0 = std::chrono::steady_clock::now();
    std::string answer = agent.run(query);
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    recorder.finalize(agent.last_run_iterations(), agent.last_run_tokens(),
                      ms, answer);
    recorder.save_to_file();

    fmt::print("=== REAL AGENT (DeepSeek + real tools) ===\n"
               "Query: {}\n"
               "Iterations: {} | Tokens: {} | Elapsed: {:.1f} ms\n\n"
               "Final answer:\n{}\n\n"
               "Trace saved to logs/real_agent.json\n",
               query, agent.last_run_iterations(), agent.last_run_tokens(),
               ms, answer);

    shutdown_python_bridge();
    return 0;
}
