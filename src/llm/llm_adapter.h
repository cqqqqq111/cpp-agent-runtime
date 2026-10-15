/**
 * @file llm_adapter.h
 * @brief C++ interface for LLM calls (multi-turn + tools).
 *
 * All LLM communication flows through this thin C++ adapter. The actual
 * HTTP/API work is delegated to Python via pybind11.
 *
 * Day 1: single string chat
 * Day 4: multi-turn messages + tools  ← current
 * Day 11: streaming, async, pooling
 */

#pragma once

#include "agent/message.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpp_agent {
class LLMTimeout : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Result of a single LLM completion call.
struct LLMResponse {
    std::string content;             // text response (may be empty)
    std::vector<ToolCall> tool_calls;// tool calls the model wants to make
    bool has_tool_calls = false;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    std::string model;
    std::string finish_reason;       // "stop", "tool_calls", "length", etc.
    bool usage_available = false;    // provider supplied usage, not an estimate

    /// Serialize to JSON for trace export (Day 16).
    json to_json() const;
};

/// Streaming token callback.
using TokenCallback = std::function<void(const std::string& token)>;

/// Abstract LLM adapter.
class LLMAdapter {
public:
    virtual ~LLMAdapter() = default;

    // A deadline must be enforced by the transport/adapter, which owns the
    // request. Never detach a thread that borrows this adapter or its callbacks.
    virtual void set_request_timeout(int milliseconds) {
        if (milliseconds != 0)
            throw std::invalid_argument("LLM adapter does not support request timeouts");
    }

    /// Day 1 backwards-compat simple call (kept for main.cpp smoke test).
    virtual LLMResponse chat(
        const std::string& system_prompt,
        const std::string& user_message
    ) = 0;

    /// Day 4+: Multi-turn chat with tools.
    virtual LLMResponse chat_full(
        const std::vector<Message>& messages,
        const json& tools
    ) = 0;

    /// Day 11+: Streaming variant. Default implementation falls back to
    /// chat_full() — override for real streaming support.
    virtual void chat_full_streaming(
        const std::vector<Message>& messages,
        const json& tools,
        const std::function<void(const std::string&)>& on_token,
        const std::function<void(LLMResponse)>& on_complete
    ) {
        auto resp = chat_full(messages, tools);
        if (!resp.content.empty() && on_token) on_token(resp.content);
        if (on_complete) on_complete(resp);
    };

    virtual std::string name() const = 0;
};

/// Create the default LLM adapter backed by Python/OpenAI.
std::unique_ptr<LLMAdapter> make_python_llm_adapter(
    const std::string& model = "gpt-4o-mini"
);

}  // namespace cpp_agent
