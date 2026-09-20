/**
 * @file message.h
 * @brief Core message types and conversation history management.
 *
 * These types form the lingua franca between the LLM, the Agent loop,
 * and the Tool system. Every component reads/writes Message objects.
 *
 * Day 3 scope:
 *   - Message struct with JSON serialization (nlohmann/json)
 *   - MessageHistory with basic token-aware truncation
 *   - ToolCall / ToolResult types
 *
 * Later days: exact token counting (tiktoken), streaming deltas, etc.
 */

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace cpp_agent {

using json = nlohmann::json;

// ============================================================================
// Message roles (matching OpenAI Chat Completions API)
// ============================================================================
namespace MessageRole {
    constexpr const char* SYSTEM    = "system";
    constexpr const char* USER      = "user";
    constexpr const char* ASSISTANT = "assistant";
    constexpr const char* TOOL      = "tool";
}  // namespace MessageRole

// ============================================================================
// ToolCall — a single function/tool invocation requested by the LLM
// ============================================================================
struct ToolCall {
    std::string id;              // unique call id from the LLM
    std::string function_name;   // name of the tool to call
    std::string arguments;       // JSON string of arguments

    /// Parse arguments as a JSON object. Returns empty optional on failure.
    std::optional<json> parsed_arguments() const noexcept;

    /// Serialize to OpenAI-compatible JSON.
    json to_json() const;
    static ToolCall from_json(const json& j);
};

// ============================================================================
// ToolResult — the output of executing a tool call
// ============================================================================
struct ToolResult {
    std::string tool_call_id;  // matches ToolCall::id
    std::string name;          // tool name (used in the role="tool" message)
    std::string content;       // result as a string
    bool success = true;       // false if the tool threw an error

    json to_json() const;
    static ToolResult from_json(const json& j);
};

// ============================================================================
// Message — a single turn in the conversation
// ============================================================================
struct Message {
    std::string role;     // "system" | "user" | "assistant" | "tool"

    // Text content. May be empty when tool_calls is present (assistant),
    // or when tool_call_id is set (tool result).
    std::string content;

    // For assistant messages that request tool calls.
    std::vector<ToolCall> tool_calls;

    // For tool result messages: which tool call does this respond to?
    std::string tool_call_id;

    // For tool result messages: the tool's registered name.
    std::string name;

    // ---- Factory helpers ---------------------------------------------------
    static Message system(const std::string& text) {
        Message m; m.role = MessageRole::SYSTEM; m.content = text; return m;
    }
    static Message user(const std::string& text) {
        Message m; m.role = MessageRole::USER; m.content = text; return m;
    }
    static Message assistant(const std::string& text) {
        Message m; m.role = MessageRole::ASSISTANT; m.content = text; return m;
    }
    static Message assistant_tool_calls(std::vector<ToolCall> calls) {
        Message m;
        m.role = MessageRole::ASSISTANT;
        m.tool_calls = std::move(calls);
        return m;
    }
    static Message tool_result(const std::string& call_id, const std::string& tool_name,
                               const std::string& output) {
        Message m;
        m.role = MessageRole::TOOL;
        m.tool_call_id = call_id;
        m.name = tool_name;
        m.content = output;
        return m;
    }

    /// Create a tool result with success flag (for registry errors, etc.).
    static Message tool_result_ex(const std::string& call_id, const std::string& tool_name,
                                  const std::string& output, bool /*success*/) {
        return tool_result(call_id, tool_name, output);
    }

    // ---- Serialization -----------------------------------------------------
    json to_json() const;
    static Message from_json(const json& j);

    // ---- Helpers -----------------------------------------------------------
    bool has_tool_calls() const noexcept { return !tool_calls.empty(); }
    bool is_tool_result() const noexcept { return role == MessageRole::TOOL; }

    /// Rough character-count estimate of this message's "token weight".
    /// Day 3 uses char count / 4 as a heuristic; Day 11 upgrades to tiktoken.
    size_t estimated_tokens() const noexcept;
};

// ============================================================================
// MessageHistory — conversation state with truncation
// ============================================================================
class MessageHistory {
public:
    MessageHistory() = default;

    /// Add a message at the end.
    void push(Message msg);

    /// Access all messages (read-only).
    const std::deque<Message>& messages() const noexcept { return messages_; }

    /// Number of stored messages.
    size_t size() const noexcept { return messages_.size(); }

    /// Total estimated token count (char / 4 heuristic for Day 3).
    size_t estimated_tokens() const noexcept;

    /// The system message, if set.
    const std::optional<Message>& system_message() const noexcept { return system_message_; }

    /// Set (or replace) the system message.
    void set_system_message(Message msg);

    /// Truncate from the front to stay under max_tokens. The system message
    /// is NEVER removed; only user/assistant/tool messages are evicted.
    /// @return number of messages removed.
    size_t truncate_to(size_t max_tokens);

    /// Remove all messages except the system message.
    void clear();

private:
    std::optional<Message> system_message_;
    std::deque<Message> messages_;   // double-ended for efficient front-truncation
    size_t total_tokens_ = 0;
};

}  // namespace cpp_agent
