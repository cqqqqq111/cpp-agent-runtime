#include "llm/llm_adapter.h"
#include "llm/py_bridge.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <stdexcept>
#include <thread>

namespace cpp_agent {

// ============================================================================
// LLMResponse serialization (Day 16: trace export)
// ============================================================================

json LLMResponse::to_json() const {
    json j;
    j["content"] = content;
    j["has_tool_calls"] = has_tool_calls;
    j["prompt_tokens"] = prompt_tokens;
    j["completion_tokens"] = completion_tokens;
    j["total_tokens"] = total_tokens;
    j["model"] = model;
    j["finish_reason"] = finish_reason;

    json tcs = json::array();
    for (const auto& tc : tool_calls) {
        tcs.push_back(tc.to_json());
    }
    j["tool_calls"] = tcs;

    return j;
}

// ============================================================================
// PythonLLMAdapter
// ============================================================================

class PythonLLMAdapter : public LLMAdapter {
public:
    explicit PythonLLMAdapter(std::string model)
        : model_(std::move(model)) {}

    // ---- Day 1 compat -------------------------------------------------------
    LLMResponse chat(
        const std::string& system_prompt,
        const std::string& user_message
    ) override {
        std::vector<Message> msgs;
        if (!system_prompt.empty()) {
            msgs.push_back(Message::system(system_prompt));
        }
        msgs.push_back(Message::user(user_message));
        return chat_full(msgs, json::array());
    }

    // ---- Day 4: full multi-turn + tools ------------------------------------
    LLMResponse chat_full(
        const std::vector<Message>& messages,
        const json& tools
    ) override {
        // Serialize messages to JSON array
        json msgs_json = json::array();
        for (const auto& m : messages) {
            msgs_json.push_back(m.to_json());
        }

        std::string msgs_str = msgs_json.dump();
        std::string tools_str = tools.is_null() ? "[]" : tools.dump();

        spdlog::debug("chat_full: {} messages, tools_count={}",
                      messages.size(), tools.is_array() ? tools.size() : 0);

        // Call the bridge with retry on transient failures.
        // Day 5: simple retry loop (max 3 attempts, linear backoff).
        // Day 11+: exponential backoff with jitter.
        constexpr int kMaxRetries = 3;
        std::string raw;
        for (int attempt = 0; attempt <= kMaxRetries; ++attempt) {
            try {
                raw = call_llm_messages_bridge(msgs_str, tools_str);
                break;  // success
            } catch (const std::exception& e) {
                if (attempt == kMaxRetries) {
                    spdlog::error("LLM call failed after {} retries: {}", kMaxRetries, e.what());
                    throw;  // rethrow after all retries exhausted
                }
                spdlog::warn("LLM call attempt {}/{} failed: {}. Retrying in {}s...",
                            attempt + 1, kMaxRetries + 1, e.what(), attempt + 1);
                std::this_thread::sleep_for(std::chrono::seconds(attempt + 1));
            }
        }

        // Parse JSON response
        json resp = json::parse(raw);

        LLMResponse result;
        result.content = resp.value("content", "");
        result.finish_reason = resp.value("finish_reason", "stop");
        result.model = resp.value("model", model_);

        if (resp.contains("usage") && !resp["usage"].is_null()) {
            result.prompt_tokens = resp["usage"].value("prompt_tokens", 0);
            result.completion_tokens = resp["usage"].value("completion_tokens", 0);
            result.total_tokens = resp["usage"].value("total_tokens", 0);
        }

        // Parse tool_calls array
        if (resp.contains("tool_calls") && !resp["tool_calls"].is_null()) {
            for (const auto& tcj : resp["tool_calls"]) {
                result.tool_calls.push_back(ToolCall::from_json(tcj));
            }
            result.has_tool_calls = !result.tool_calls.empty();
        }

        spdlog::debug("chat_full response: finish={}, content_len={}, tool_calls={}",
                      result.finish_reason, result.content.size(), result.tool_calls.size());

        return result;
    }

    // ---- Day 11: streaming --------------------------------------------------
    void chat_full_streaming(
        const std::vector<Message>& messages,
        const json& tools,
        const std::function<void(const std::string&)>& on_token,
        const std::function<void(LLMResponse)>& on_complete
    ) override {
        json msgs_json = json::array();
        for (const auto& m : messages) msgs_json.push_back(m.to_json());

        std::string msgs_str = msgs_json.dump();
        std::string tools_str = tools.is_null() ? "[]" : tools.dump();

        spdlog::debug("chat_full_streaming: {} messages, tools={}",
                      messages.size(), tools.is_array() ? tools.size() : 0);

        call_llm_streaming_bridge(msgs_str, tools_str, on_token,
            [on_complete, this](const std::string& raw) {
                json resp = json::parse(raw);
                LLMResponse result;
                result.content = resp.value("content", "");
                result.finish_reason = resp.value("finish_reason", "stop");
                result.model = resp.value("model", model_);
                if (resp.contains("usage") && !resp["usage"].is_null()) {
                    result.prompt_tokens = resp["usage"].value("prompt_tokens", 0);
                    result.completion_tokens = resp["usage"].value("completion_tokens", 0);
                    result.total_tokens = resp["usage"].value("total_tokens", 0);
                }
                // Day 19: parse tool_calls accumulated by the Python stream.
                if (resp.contains("tool_calls") && !resp["tool_calls"].is_null()) {
                    for (const auto& tcj : resp["tool_calls"]) {
                        result.tool_calls.push_back(ToolCall::from_json(tcj));
                    }
                    result.has_tool_calls = !result.tool_calls.empty();
                }
                on_complete(result);
            });
    }

    std::string name() const override {
        return "PythonLLMAdapter(" + model_ + ")";
    }

private:
    std::string model_;
};

// ---- Factory ----------------------------------------------------------------

std::unique_ptr<LLMAdapter> make_python_llm_adapter(const std::string& model) {
    return std::make_unique<PythonLLMAdapter>(model);
}

}  // namespace cpp_agent
