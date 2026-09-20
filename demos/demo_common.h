/**
 * @file demo_common.h
 * @brief Shared plumbing for Day 21 demo scenarios (code review / debug / refactor).
 *
 * Each demo is a self-contained executable that runs a *scripted* agent:
 * a canned MockLLM drives a real ReActLoop over real tools (filesystem / grep /
 * git), and a TraceRecorder captures the whole reasoning chain to a JSON file
 * viewable in web/index.html. No real LLM, no API key — fully offline and
 * deterministic, mirroring the benchmark approach in benchmarks/bench_react.cpp.
 */

#pragma once

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "tool/tool_base.h"
#include "tracing/trace_recorder.h"

#include <fmt/core.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace demo {

// ---- Scripted LLM ---------------------------------------------------------

struct ScriptedResponse {
    bool has_tool_calls = false;
    std::string content;                       // final answer (when has_tool_calls=false)
    std::vector<cpp_agent::ToolCall> tool_calls;
    std::string finish_reason = "stop";
};

/// Returns canned responses in order; each ReAct iteration consumes one.
class ScriptedLLM : public cpp_agent::LLMAdapter {
public:
    explicit ScriptedLLM(std::vector<ScriptedResponse> script)
        : script_(std::move(script)) {}

    cpp_agent::LLMResponse chat(const std::string&, const std::string&) override {
        return next();
    }
    cpp_agent::LLMResponse chat_full(const std::vector<cpp_agent::Message>&,
                                     const cpp_agent::json&) override {
        return next();
    }
    std::string name() const override { return "ScriptedLLM"; }

private:
    cpp_agent::LLMResponse next() {
        cpp_agent::LLMResponse r;
        if (current_ < script_.size()) {
            auto& s = script_[current_++];
            r.content = s.content;
            r.has_tool_calls = s.has_tool_calls;
            r.tool_calls = s.tool_calls;
            r.finish_reason = s.finish_reason;
            r.total_tokens = 100;
            r.prompt_tokens = 80;
            r.completion_tokens = 20;
            r.model = "scripted";
        } else {
            r.content = "(script exhausted)";
            r.finish_reason = "stop";
        }
        return r;
    }

    std::vector<ScriptedResponse> script_;
    size_t current_ = 0;
};

inline cpp_agent::ToolCall make_call(const std::string& id, const std::string& fn,
                                     const std::string& args_json) {
    cpp_agent::ToolCall tc;
    tc.id = id;
    tc.function_name = fn;
    tc.arguments = args_json;
    return tc;
}

// ---- Run one scripted demo, record a trace, print a summary -----------------

/// @param name          Demo name (also the trace filename stem: logs/<name>.json).
/// @param query         The user's natural-language request.
/// @param system_prompt Agent role prompt.
/// @param script        The canned LLM responses (tool rounds + final answer).
/// @param register_tools  Populate the ToolRegistry with real tools.
inline int run_demo(const std::string& name,
                    const std::string& query,
                    const std::string& system_prompt,
                    std::vector<ScriptedResponse> script,
                    std::function<void(cpp_agent::ToolRegistry&)> register_tools) {
    using namespace cpp_agent;

    ToolRegistry registry;
    register_tools(registry);

    AgentConfig cfg;
    cfg.system_prompt = system_prompt;
    cfg.max_iterations = 10;

    ScriptedLLM llm(std::move(script));
    ReActLoop agent(llm, registry, cfg);

    std::string out_path = "logs/" + name + ".json";
    TraceRecorder recorder(out_path);
    recorder.set_config(cfg);
    recorder.set_query(query);
    agent.set_trace_callback(recorder.wrap({}));

    auto t0 = std::chrono::steady_clock::now();
    std::string answer = agent.run(query);
    double elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    recorder.finalize(agent.last_run_iterations(), agent.last_run_tokens(),
                      elapsed, answer);
    recorder.save_to_file();

    fmt::print("=== [{}] demo ===\nQuery: {}\n\nFinal answer:\n{}\n\n"
               "Iterations: {} | Tools: {} | Elapsed: {:.2f} ms\n"
               "Trace saved to: {}\n\n",
               name, query, answer,
               agent.last_run_iterations(),
               registry.size(),
               elapsed, out_path);
    return 0;
}

}  // namespace demo
