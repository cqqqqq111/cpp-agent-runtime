/**
 * @file bench_react.cpp
 * @brief Day 20 Task 3 — ReAct loop end-to-end benchmark (MockLLM).
 *
 * Uses a scripted MockLLMAdapter (no network, deterministic) so the numbers
 * isolate the *loop machinery* — message building, tool execution, truncation —
 * rather than LLM latency. Three measurements:
 *
 *   1. Wall-clock across 1 / 5 / 10 loop iterations
 *   2. Parallel vs serial tool execution at 1 / 2 / 4 / 8 tool calls
 *   3. Token-truncation overhead at 100% / 90% / 80% context budget
 */

#include "bench_common.h"
#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "tool/echo_tool.h"
#include "tool/tool_base.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace cpp_agent;

// ============================================================================
// MockLLMAdapter — canned responses (mirrors tests/test_react_loop.cpp)
// ============================================================================
class MockLLMAdapter : public LLMAdapter {
public:
    struct ScriptedResponse {
        bool has_tool_calls = false;
        std::string content;
        std::vector<ToolCall> tool_calls;
        std::string finish_reason = "stop";
    };
    explicit MockLLMAdapter(std::vector<ScriptedResponse> script)
        : script_(std::move(script)) {}

    LLMResponse chat(const std::string&, const std::string&) override {
        LLMResponse r; r.content = "(mock: use chat_full)"; r.finish_reason = "stop"; return r;
    }
    LLMResponse chat_full(const std::vector<Message>&, const json&) override {
        if (current_ < script_.size()) {
            auto& s = script_[current_++];
            LLMResponse r;
            r.content = s.content;
            r.has_tool_calls = s.has_tool_calls;
            r.tool_calls = s.tool_calls;
            r.finish_reason = s.finish_reason;
            r.total_tokens = 100;
            return r;
        }
        LLMResponse fb; fb.content = "(mock exhausted)"; fb.finish_reason = "stop"; return fb;
    }
    std::string name() const override { return "MockLLM"; }
private:
    std::vector<ScriptedResponse> script_;
    size_t current_ = 0;
};

static ToolCall make_call(const std::string& id, const std::string& fn,
                          const std::string& args) {
    ToolCall tc;
    tc.id = id; tc.function_name = fn; tc.arguments = args;
    return tc;
}

// A tool that sleeps 1ms — gives tool execution real, deterministic cost so
// serial-vs-parallel scaling is visible. Parallel mode runs these on worker
// threads, so N concurrent sleeps overlap instead of summing.
class SleepTool : public Tool {
public:
    std::string name() const override { return "sleep"; }
    std::string description() const override { return "sleep 1ms"; }
    json parameters_schema() const override { return {{"type", "object"}, {"properties", json::object()}}; }
    ToolResult execute(const json&, const std::string& call_id) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ToolResult tr; tr.tool_call_id = call_id; tr.name = name();
        tr.content = "slept 1ms"; tr.success = true;
        return tr;
    }
};

// ============================================================================
// 1. Wall-clock across 1 / 5 / 10 iterations
// ============================================================================
static void bench_iterations() {
    std::cout << "--- ReAct loop wall-clock (MockLLM, echo tool) ---\n";
    bench::print_header();

    for (int iters : {1, 5, 10}) {
        // Script: (iters-1) tool-call rounds + 1 final answer = `iters` LLM calls.
        std::vector<MockLLMAdapter::ScriptedResponse> script;
        for (int i = 0; i < iters - 1; ++i) {
            script.push_back({true, "", {make_call("c_" + std::to_string(i), "echo",
                                                   "{\"message\":\"x" + std::to_string(i) + "\"}")},
                              "tool_calls"});
        }
        script.push_back({false, "final answer", {}, "stop"});

        auto st = bench::run_reps([&] {
            MockLLMAdapter mock(script);
            ToolRegistry reg;
            reg.register_tool(std::make_unique<EchoTool>());
            AgentConfig cfg;
            cfg.max_iterations = static_cast<size_t>(iters + 2);
            ReActLoop agent(mock, reg, cfg);
            agent.run("bench query");
        }, 20);

        bench::print_row("react " + std::to_string(iters) + " iteration(s)", st);
    }
}

// ============================================================================
// 2. Parallel vs serial tool execution
// ============================================================================
static void bench_parallel_serial() {
    std::cout << "\n--- parallel vs serial tool execution (SleepTool 1ms) ---\n";
    bench::print_header();

    for (int n : {1, 2, 4, 8}) {
        // One iteration requesting N sleep calls, then a final answer.
        auto make_script = [&] {
            std::vector<ToolCall> calls;
            for (int i = 0; i < n; ++i) {
                calls.push_back(make_call("s_" + std::to_string(i), "sleep", "{}"));
            }
            return std::vector<MockLLMAdapter::ScriptedResponse>{
                {true, "", calls, "tool_calls"},
                {false, "done", {}, "stop"},
            };
        };

        // Serial: no scheduler attached.
        auto serial = bench::run_reps([&] {
            MockLLMAdapter mock(make_script());
            ToolRegistry reg;
            reg.register_tool(std::make_unique<SleepTool>());
            AgentConfig cfg;
            ReActLoop agent(mock, reg, cfg);
            agent.run("bench");
        }, 10);
        bench::print_row("N=" + std::to_string(n) + " serial", serial);

        // Parallel: tool calls dispatched onto worker threads.
        auto parallel = bench::run_reps([&] {
            MockLLMAdapter mock(make_script());
            ToolRegistry reg;
            reg.register_tool(std::make_unique<SleepTool>());
            AgentConfig cfg;
            ReActLoop agent(mock, reg, cfg);
            agent.set_parallel_tools(true);
            agent.run("bench");
        }, 10);
        bench::print_row("N=" + std::to_string(n) + " parallel", parallel);
    }
}

// ============================================================================
// 3. Token-truncation overhead
// ============================================================================
static void bench_truncation() {
    std::cout << "\n--- token truncation (MessageHistory::truncate_to) ---\n";
    bench::print_header();

    // A large history: 4000 messages × ~400 chars ≈ 100 tokens each
    // (char/4 heuristic) → ~400K tokens before truncation.
    const size_t kMsgs = 4000;
    const std::string body(400, 'x');
    MessageHistory ref;
    for (size_t i = 0; i < kMsgs; ++i) {
        ref.push(Message::user(body + std::to_string(i)));
    }
    const size_t full = ref.estimated_tokens();

    struct Case { const char* label; double frac; };
    for (const auto& c : {Case{"truncate to 100% budget", 1.00},
                          Case{"truncate to 90% budget", 0.90},
                          Case{"truncate to 80% budget", 0.80}}) {
        size_t budget = static_cast<size_t>(static_cast<double>(full) * c.frac);
        // Time ONLY truncate_to (history build is untimed); truncate mutates,
        // so rebuild a fresh history before each timed call.
        std::vector<double> samples;
        samples.reserve(20);
        for (int rep = 0; rep < 20; ++rep) {
            MessageHistory h;
            for (size_t i = 0; i < kMsgs; ++i) {
                h.push(Message::user(body + std::to_string(i)));
            }
            auto t0 = bench::Clock::now();
            h.truncate_to(budget);
            samples.push_back(bench::elapsed_ms(t0));
        }
        bench::print_row(c.label, bench::summarize(std::move(samples)));
    }
    std::cout << "  (history ≈ " << full << " tokens; 90%/80% evict ~400/~800 msgs)\n";
}

int main() {
    spdlog::set_level(spdlog::level::off);  // keep benchmark output clean
    std::cout << "=== ReAct loop end-to-end (ms, lower is better) ===\n\n";
    bench_iterations();
    bench_parallel_serial();
    bench_truncation();
    std::cout << "\nDone.\n";
    return 0;
}
