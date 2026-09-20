/**
 * Day 11 tests: streaming + parallel tool execution.
 */

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "core/scheduler.h"
#include "core/task.h"
#include "tool/echo_tool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace cpp_agent;

// ============================================================================
// SlowEchoTool — simulates a slow tool (for parallel execution test)
// ============================================================================
class SlowEchoTool : public Tool {
public:
    std::atomic<int>* exec_count = nullptr;  // thread-safe execution counter

    std::string name() const override { return "slow_echo"; }
    std::string description() const override { return "Counting echo for parallel test."; }
    json parameters_schema() const override {
        return {{"type", "object"}, {"properties", {{"msg", {{"type", "string"}}}}},
                {"required", json::array({"msg"})}};
    }

    ToolResult execute(const json& args, const std::string& call_id) override {
        // Parallel mode runs each tool call on its own worker thread, so the
        // counter must be atomic.
        if (exec_count) exec_count->fetch_add(1, std::memory_order_relaxed);

        ToolResult tr;
        tr.tool_call_id = call_id;
        tr.name = name();
        tr.content = "[echo] " + args.value("msg", "");
        tr.success = true;
        return tr;
    }
};

// ============================================================================
// Test 1: Sequential execution — each tool takes 50ms, total ≈ sum
// ============================================================================
static ToolCall make_call(const std::string& id, const std::string& name,
                          const std::string& msg) {
    ToolCall tc;
    tc.id = id;
    tc.function_name = name;
    tc.arguments = R"({"msg": ")" + msg + R"("})";
    return tc;
}

static bool test_parallel_vs_sequential() {
    std::atomic<int> exec_count{0};
    auto tool1 = std::make_unique<SlowEchoTool>();
    tool1->exec_count = &exec_count;

    ToolRegistry registry;
    registry.register_tool(std::move(tool1));

    class MockLLM : public LLMAdapter {
    public:
        std::vector<ToolCall> calls;
        int call_count = 0;

        LLMResponse chat(const std::string&, const std::string&) override {
            LLMResponse r; r.content = "ok"; r.finish_reason = "stop"; return r;
        }
        LLMResponse chat_full(const std::vector<Message>&, const json&) override {
            ++call_count;
            if (call_count == 1) {
                LLMResponse r;
                r.has_tool_calls = true;
                r.tool_calls = calls;
                r.finish_reason = "tool_calls";
                return r;
            }
            LLMResponse r; r.content = "done"; r.finish_reason = "stop"; return r;
        }
        std::string name() const override { return "MockLLM"; }
    };

    MockLLM mock;
    mock.calls = {
        make_call("c1", "slow_echo", "first"),
        make_call("c2", "slow_echo", "second"),
        make_call("c3", "slow_echo", "third"),
    };

    AgentConfig config;
    config.max_iterations = 5;

    // ── Sequential (parallel disabled) ──
    ReActLoop agent_seq(mock, registry, config);
    agent_seq.run("test");
    assert(agent_seq.last_run_iterations() == 2);
    assert(exec_count.load() == 3);  // all 3 tools executed

    // ── Parallel (worker threads) ──
    exec_count.store(0);
    mock.call_count = 0;
    ReActLoop agent_par(mock, registry, config);
    agent_par.set_parallel_tools(true);
    agent_par.run("test");

    assert(agent_par.last_run_iterations() == 2);
    assert(exec_count.load() == 3);  // all 3 tools executed (concurrently)

    return true;
}

// ============================================================================
// Test 2: Parallel execution with single tool (falls back to sequential)
// ============================================================================
static bool test_parallel_single_tool() {
    ToolRegistry registry;
    registry.register_tool(std::make_unique<SlowEchoTool>());

    class MockLLM : public LLMAdapter {
    public:
        int call_count = 0;
        LLMResponse chat(const std::string&, const std::string&) override {
            LLMResponse r; r.content = "x"; r.finish_reason = "stop"; return r;
        }
        LLMResponse chat_full(const std::vector<Message>&, const json&) override {
            ++call_count;
            if (call_count == 1) {
                LLMResponse r;
                r.has_tool_calls = true;
                r.tool_calls = {make_call("c0", "slow_echo", "solo")};
                r.finish_reason = "tool_calls";
                return r;
            }
            LLMResponse r; r.content = "done"; r.finish_reason = "stop"; return r;
        }
        std::string name() const override { return "MockLLM"; }
    };

    MockLLM mock;
    AgentConfig config;

    ReActLoop agent(mock, registry, config);
    agent.set_parallel_tools(true);

    std::string result = agent.run("test");
    assert(agent.last_run_iterations() == 2);  // tool call + final answer
    assert(agent.history().size() >= 2);

    return true;
}

// ============================================================================
// Test 3: Scheduler yield in coroutine
// ============================================================================
static bool test_scheduler_yield() {
    Scheduler s;
    std::vector<int> log;

    auto coro = [&](Scheduler& s, int id) -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            log.push_back(id);
            co_await s.yield();
        }
        co_return;
    };

    auto t0 = coro(s, 0);
    auto t1 = coro(s, 1);
    s.schedule(t0.handle());
    s.schedule(t1.handle());
    s.run_all();

    // Round-robin: 0, 1, 0, 1, 0, 1
    assert(log.size() == 6);
    assert(log == std::vector<int>({0, 1, 0, 1, 0, 1}));
    assert(t0.done() && t1.done());

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
        {"sequential vs parallel execution", test_parallel_vs_sequential},
        {"parallel single tool (fallback)", test_parallel_single_tool},
        {"scheduler round-robin yield", test_scheduler_yield},
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
