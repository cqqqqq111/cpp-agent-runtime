/**
 * @file react_loop.h
 * @brief ReAct agent loop — the core "brain" of the agent engine.
 *
 * The ReAct pattern (Reasoning + Acting) is the standard agent control flow:
 *
 *   1. THINK:  LLM receives conversation + tool list, decides what to do
 *   2. ACT:    If LLM returns tool_calls → execute tools via ToolRegistry
 *   3. OBSERVE: Append tool results as messages, loop back to step 1
 *
 * The loop terminates when the LLM returns a text response with no tool
 * calls, or when max_iterations is exhausted.
 *
 * Usage:
 *   ReActLoop agent(llm, registry, config);
 *   std::string answer = agent.run("帮我查一下天气");
 */

#pragma once

#include "agent/agent_config.h"
#include "agent/message.h"
#include "llm/llm_adapter.h"
#include "tool/tool_base.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cpp_agent {

class WorkingMemory;  // forward

// ============================================================================
// TraceStep — records one iteration of the loop (for debugging / visualization)
// ============================================================================
struct TraceStep {
    enum class Phase { THINK, ACT, OBSERVE };

    int iteration = 0;
    Phase phase = Phase::THINK;
    std::string detail;              // human-readable description
    std::vector<ToolCall> tool_calls; // what the LLM asked for (ACT phase)
    std::vector<ToolResult> tool_results; // what the tools returned (OBSERVE phase)
    std::string llm_content;         // LLM's text response (THINK phase)

    // ---- Day 16: trace visualization fields ----
    /// High-precision timestamp set when the step is created.
    std::chrono::system_clock::time_point timestamp;

    /// Full LLM response metadata (only meaningful during THINK phase).
    std::optional<LLMResponse> llm_response;

    /// Serialize to JSON for trace export.
    json to_json() const;
};

// ============================================================================
// ReActLoop
// ============================================================================
class ReActLoop {
public:
    /// Callback invoked after each trace step (for logging, TUI, visualization).
    using TraceCallback = std::function<void(const TraceStep&)>;

    /// Construct with required dependencies.
    /// @param llm      The LLM adapter (already initialized).
    /// @param registry The tool registry (tools pre-registered).
    /// @param config   Agent configuration.
    ReActLoop(LLMAdapter& llm, ToolRegistry& registry, const AgentConfig& config);

    /// Run the ReAct loop for a single user query.
    /// @param user_input  The user's question / request.
    /// @returns The agent's final text answer.
    std::string run(const std::string& user_input);

    /// Day 18: run the loop with streaming — `on_token` is called for each
    /// text token as it arrives (for the TUI typewriter effect).
    /// @param on_token  Token callback (may be empty to discard streaming).
    std::string run_streaming(const std::string& user_input,
                              std::function<void(const std::string&)> on_token);

    /// Set a callback for trace events.
    void set_trace_callback(TraceCallback cb) { trace_cb_ = std::move(cb); }

    /// Attach a working memory store.
    void set_working_memory(WorkingMemory* wm) { working_memory_ = wm; }

    /// Enable parallel tool execution. When enabled, multiple tool calls in a
    /// single iteration are dispatched onto worker threads and run concurrently,
    /// giving real wall-clock speedup for blocking tools. Disabled by default.
    void set_parallel_tools(bool enable = true) { parallel_tools_ = enable; }

    /// Access the full conversation history after a run.
    const MessageHistory& history() const noexcept { return history_; }

    /// Number of LLM calls made in the last run.
    size_t last_run_iterations() const noexcept { return last_iterations_; }

    /// Total tokens consumed in the last run.
    size_t last_run_tokens() const noexcept { return last_tokens_; }

private:
    LLMAdapter& llm_;
    ToolRegistry& registry_;
    AgentConfig config_;
    TraceCallback trace_cb_;
    WorkingMemory* working_memory_ = nullptr;
    bool parallel_tools_ = false;

    MessageHistory history_;
    size_t last_iterations_ = 0;
    size_t last_tokens_ = 0;

    // ---- Day 18: loop detection state ----
    std::string last_fingerprint_;                  // fingerprint of the previous iteration
    std::vector<std::string> recent_fingerprints_;  // last 3, for ABAB oscillation

    /// Execute all tool calls from an LLM response, return result messages.
    /// @param out_trace_results  If non-null, populated with ToolResult for tracing.
    std::vector<Message> execute_tools(const std::vector<ToolCall>& calls,
                                        std::vector<ToolResult>* out_trace_results = nullptr);

    /// Shared loop body; `streaming` selects chat_full vs chat_full_streaming.
    std::string run_impl(const std::string& user_input,
                         const std::function<void(const std::string&)>& on_token,
                         bool streaming);

    /// Block until a streaming LLM call completes, then return its response.
    LLMResponse llm_stream_once(const std::vector<Message>& msgs,
                                const json& tools,
                                const std::function<void(const std::string&)>& on_token);

    /// Day 18: fingerprint the requested tool calls and check for a repeated /
    /// oscillating pattern. Returns a stop message, or std::nullopt if healthy.
    std::optional<std::string> detect_loop(const std::vector<ToolCall>& calls);

    /// Build the tools array for the LLM request.
    json build_tools_json();
};

}  // namespace cpp_agent
