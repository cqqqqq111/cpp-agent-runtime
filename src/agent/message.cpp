#include "agent/message.h"

#include <algorithm>
#include <stdexcept>

namespace cpp_agent {

// ============================================================================
// ToolCall
// ============================================================================

std::optional<json> ToolCall::parsed_arguments() const noexcept {
    try {
        return json::parse(arguments);
    } catch (...) {
        return std::nullopt;
    }
}

json ToolCall::to_json() const {
    return {
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", function_name},
            {"arguments", arguments}
        }}
    };
}

ToolCall ToolCall::from_json(const json& j) {
    ToolCall tc;
    tc.id = j.at("id").get<std::string>();
    tc.function_name = j.at("function").at("name").get<std::string>();
    tc.arguments = j.at("function").at("arguments").get<std::string>();
    return tc;
}

// ============================================================================
// ToolResult
// ============================================================================

json ToolResult::to_json() const {
    return {
        {"tool_call_id", tool_call_id},
        {"name", name},
        {"content", content},
        {"success", success}
    };
}

ToolResult ToolResult::from_json(const json& j) {
    ToolResult tr;
    tr.tool_call_id = j.at("tool_call_id").get<std::string>();
    tr.name = j.at("name").get<std::string>();
    tr.content = j.at("content").get<std::string>();
    tr.success = j.value("success", true);
    return tr;
}

// ============================================================================
// Message
// ============================================================================

json Message::to_json() const {
    json j;
    j["role"] = role;

    if (!content.empty()) {
        j["content"] = content;
    } else if (!tool_calls.empty()) {
        j["content"] = nullptr;
    }

    if (!tool_calls.empty()) {
        json tcs = json::array();
        for (const auto& tc : tool_calls) {
            tcs.push_back(tc.to_json());
        }
        j["tool_calls"] = tcs;
    }

    if (!tool_call_id.empty()) {
        j["tool_call_id"] = tool_call_id;
    }
    if (!name.empty()) {
        j["name"] = name;
    }

    return j;
}

Message Message::from_json(const json& j) {
    Message m;
    m.role = j.at("role").get<std::string>();

    if (j.contains("content") && !j["content"].is_null()) {
        m.content = j["content"].get<std::string>();
    }

    if (j.contains("tool_calls") && !j["tool_calls"].is_null()) {
        for (const auto& tcj : j["tool_calls"]) {
            m.tool_calls.push_back(ToolCall::from_json(tcj));
        }
    }

    m.tool_call_id = j.value("tool_call_id", "");
    m.name = j.value("name", "");

    return m;
}

size_t Message::estimated_tokens() const noexcept {
    // Day 3 heuristic: ~4 characters per token. Day 11+ upgrades to tiktoken.
    size_t chars = content.size();
    for (const auto& tc : tool_calls) {
        chars += tc.function_name.size() + tc.arguments.size();
    }
    // Each tool call adds ~50 tokens of structural overhead in the API format
    size_t tool_overhead = tool_calls.size() * 50;
    return std::max(size_t(1), chars / 4 + tool_overhead);
}

// ============================================================================
// MessageHistory
// ============================================================================

void MessageHistory::push(Message msg) {
    size_t tok = msg.estimated_tokens();
    messages_.push_back(std::move(msg));
    total_tokens_ += tok;
}

size_t MessageHistory::estimated_tokens() const noexcept {
    size_t total = total_tokens_;
    if (system_message_) {
        total += system_message_->estimated_tokens();
    }
    return total;
}

void MessageHistory::set_system_message(Message msg) {
    system_message_ = std::move(msg);
}

size_t MessageHistory::truncate_to(size_t max_tokens) {
    size_t removed = 0;

    while (estimated_tokens() > max_tokens && !messages_.empty()) {
        total_tokens_ -= messages_.front().estimated_tokens();
        messages_.pop_front();
        ++removed;
        // Never retain orphan tool results after removing their assistant call.
        while (!messages_.empty() && messages_.front().is_tool_result()) {
            total_tokens_ -= messages_.front().estimated_tokens();
            messages_.pop_front();
            ++removed;
        }
    }

    return removed;
}

void MessageHistory::clear() {
    messages_.clear();
    total_tokens_ = 0;
    // system_message_ is preserved
}

}  // namespace cpp_agent
