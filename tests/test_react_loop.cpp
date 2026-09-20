/**
 * Day 4 tests: ReAct loop with mock LLM adapter.
 *
 * We use a MockLLMAdapter to control exact LLM responses, avoiding
 * the cost and non-determinism of real API calls.
 */

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "tool/echo_tool.h"
#include "tool/tool_base.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <vector>

using namespace cpp_agent;

// ============================================================================
// MockLLMAdapter — returns canned responses for testing
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

    LLMResponse chat(const std::string& /*sys*/, const std::string& /*user*/) override {
        LLMResponse r;
        r.content = "(mock: use chat_full)";
        r.finish_reason = "stop";
        return r;
    }

    LLMResponse chat_full(const std::vector<Message>& /*msgs*/,
                          const json& /*tools*/) override {
        if (current_ < script_.size()) {
            auto& s = script_[current_++];
            LLMResponse resp;
            resp.content = s.content;
            resp.has_tool_calls = s.has_tool_calls;
            resp.tool_calls = s.tool_calls;
            resp.finish_reason = s.finish_reason;
            resp.total_tokens = 100;
            return resp;
        }
        // Fallback: return empty response
        LLMResponse fallback;
        fallback.content = "(mock exhausted)";
        fallback.finish_reason = "stop";
        fallback.total_tokens = 10;
        return fallback;
    }

    std::string name() const override { return "MockLLM"; }

    size_t calls_made() const { return current_; }

private:
    std::vector<ScriptedResponse> script_;
    size_t current_ = 0;
};

// ============================================================================
// Helper: build a simple ToolCall
// ============================================================================
static ToolCall make_echo_call(const std::string& id, const std::string& message) {
    ToolCall tc;
    tc.id = id;
    tc.function_name = "echo";
    tc.arguments = R"({"message": ")" + message + R"("})";
    return tc;
}

// ============================================================================
// Test 1: Simple Q&A without tools
// ============================================================================
static bool test_simple_qa_no_tools() {
    MockLLMAdapter mock({
        {false, "Hello! How can I help you today?", {}, "stop"},
    });

    ToolRegistry registry;
    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("Hi there!");

    assert(result == "Hello! How can I help you today?");
    assert(agent.last_run_iterations() == 1);  // 1 LLM call
    assert(agent.history().size() >= 2);       // user + assistant
    assert(mock.calls_made() == 1);

    return true;
}

// ============================================================================
// Test 2: LLM calls EchoTool → gets result → produces final answer
// ============================================================================
static bool test_tool_calling_flow() {
    // Script:
    //   Call 1: LLM decides to call echo tool
    //   Call 2: LLM gets tool result, produces final answer
    MockLLMAdapter mock({
        {true, "", {make_echo_call("call_1", "hello echo")}, "tool_calls"},
        {false, "The echo tool returned: [echo] hello echo", {}, "stop"},
    });

    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("Please echo 'hello echo'");

    assert(result == "The echo tool returned: [echo] hello echo");
    assert(agent.last_run_iterations() == 2);  // 2 LLM calls
    assert(mock.calls_made() == 2);

    // Verify conversation history has tool messages
    bool has_tool_result = false;
    for (const auto& m : agent.history().messages()) {
        if (m.role == MessageRole::TOOL) {
            has_tool_result = true;
            assert(m.content == "[echo] hello echo");
        }
    }
    assert(has_tool_result);

    return true;
}

// ============================================================================
// Test 3: Max iterations guard
// ============================================================================
static bool test_max_iterations_guard() {
    // Script: LLM keeps asking for tools forever, but with DIFFERENT arguments
    // each iteration so loop detection doesn't fire — this tests max_iter only.
    std::vector<MockLLMAdapter::ScriptedResponse> script;
    for (int i = 0; i < 20; ++i) {
        script.push_back({true, "", {make_echo_call("c_" + std::to_string(i),
                                                    "x" + std::to_string(i))}, "tool_calls"});
    }

    MockLLMAdapter mock(script);
    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 3;  // small limit

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("echo forever");

    assert(agent.last_run_iterations() == 3);
    assert(!result.empty());
    // Should not crash, should return something (even if it's the fallback)

    return true;
}

// ============================================================================
// Test 4: Tool not found → LLM sees error
// ============================================================================
static bool test_tool_not_found() {
    ToolCall bad_call;
    bad_call.id = "call_x";
    bad_call.function_name = "nonexistent_tool";
    bad_call.arguments = R"({"x": 1})";

    MockLLMAdapter mock({
        {true, "", {bad_call}, "tool_calls"},
        {false, "I tried to call a tool but it doesn't exist.", {}, "stop"},
    });

    ToolRegistry registry;  // empty—no tools registered

    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("do something");

    // Verify tool error was captured in history
    bool has_error = false;
    for (const auto& m : agent.history().messages()) {
        if (m.role == MessageRole::TOOL && m.name == "nonexistent_tool") {
            has_error = true;
            assert(m.content.find("not registered") != std::string::npos);
        }
    }
    assert(has_error);

    return true;
}

// ============================================================================
// Test 5: Trace callback records all steps
// ============================================================================
static bool test_trace_callback() {
    MockLLMAdapter mock({
        {true, "", {make_echo_call("call_1", "trace me")}, "tool_calls"},
        {false, "Final answer after tracing", {}, "stop"},
    });

    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);

    std::vector<TraceStep> trace;
    agent.set_trace_callback([&](const TraceStep& ts) {
        trace.push_back(ts);
    });

    std::string result = agent.run("trace test");
    assert(!result.empty());

    // Expected trace:
    // Iteration 0: THINK (tool_calls), ACT (1 tool), OBSERVE (1 result)
    // Iteration 1: THINK (final answer)
    assert(trace.size() >= 4);

    // Verify phases are in order
    assert(trace[0].phase == TraceStep::Phase::THINK);
    assert(trace[1].phase == TraceStep::Phase::ACT);
    assert(trace[2].phase == TraceStep::Phase::OBSERVE);
    assert(trace[3].phase == TraceStep::Phase::THINK);  // final answer

    return true;
}

// ============================================================================
// Test 6: System prompt is set correctly
// ============================================================================
static bool test_system_prompt() {
    MockLLMAdapter mock({
        {false, "I am a code reviewer.", {}, "stop"},
    });

    ToolRegistry registry;
    AgentConfig config;
    config.system_prompt = "You are a code reviewer. Be thorough.";

    ReActLoop agent(mock, registry, config);
    agent.run("review this");

    assert(agent.history().system_message().has_value());
    assert(agent.history().system_message()->content.find("code reviewer") != std::string::npos);

    return true;
}

// ============================================================================
// Test 7: Loop detection (Day 18) — same tool call twice → auto stop
// ============================================================================
static bool test_loop_detection() {
    // Script: LLM keeps asking for the SAME echo call (identical arguments).
    std::vector<MockLLMAdapter::ScriptedResponse> script;
    for (int i = 0; i < 20; ++i) {
        script.push_back({true, "", {make_echo_call("c_" + std::to_string(i), "loop")}, "tool_calls"});
    }

    MockLLMAdapter mock(script);
    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 10;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("echo loop forever");

    // Stops after 2 LLM calls (1st call + 1 repeat detected), not 10.
    assert(agent.last_run_iterations() == 2);
    assert(result.find("loop") != std::string::npos);

    return true;
}

// ============================================================================
// Test 8: Loop detection (ABAB oscillation) — two calls flip-flopping
// ============================================================================
static bool test_loop_detection_abab() {
    // A → B → A → B oscillation between two distinct echo calls.
    std::vector<MockLLMAdapter::ScriptedResponse> script;
    for (int i = 0; i < 20; ++i) {
        std::string msg = (i % 2 == 0) ? "alpha" : "beta";
        script.push_back({true, "", {make_echo_call("c_" + std::to_string(i), msg)}, "tool_calls"});
    }

    MockLLMAdapter mock(script);
    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 10;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run("oscillate");

    // A, B, A, B → detected on the 4th LLM call.
    assert(agent.last_run_iterations() == 4);
    assert(result.find("loop") != std::string::npos);

    return true;
}

// ============================================================================
// Test 9: run_streaming() emits tokens via the callback (typewriter)
// ============================================================================
static bool test_streaming_token_callback() {
    MockLLMAdapter mock({
        {false, "Hello streaming", {}, "stop"},
    });

    ToolRegistry registry;
    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);

    std::string collected;
    std::string result = agent.run_streaming("hi", [&](const std::string& token) {
        collected += token;
    });

    assert(result == "Hello streaming");
    // Base-class fallback emits the whole content as one "token".
    assert(collected == "Hello streaming");

    return true;
}

// ============================================================================
// Test 10: run_streaming() still executes tool calls (streaming loop)
// ============================================================================
static bool test_streaming_with_tool_calls() {
    MockLLMAdapter mock({
        {true, "", {make_echo_call("call_s1", "stream tool")}, "tool_calls"},
        {false, "Streamed tool answer", {}, "stop"},
    });

    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    AgentConfig config;
    config.max_iterations = 5;

    ReActLoop agent(mock, registry, config);
    std::string result = agent.run_streaming("echo stream", [](const std::string&) {});

    assert(result == "Streamed tool answer");
    assert(agent.last_run_iterations() == 2);

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
        {"simple Q&A without tools", test_simple_qa_no_tools},
        {"tool-calling flow (LLM → echo → answer)", test_tool_calling_flow},
        {"max iterations guard", test_max_iterations_guard},
        {"loop detection (exact repeat)", test_loop_detection},
        {"loop detection (ABAB oscillation)", test_loop_detection_abab},
        {"run_streaming token callback", test_streaming_token_callback},
        {"run_streaming with tool calls", test_streaming_with_tool_calls},
        {"tool not found → error in history", test_tool_not_found},
        {"trace callback records all phases", test_trace_callback},
        {"system prompt is preserved", test_system_prompt},
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
