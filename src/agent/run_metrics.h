#pragma once
#include "agent/message.h"
#include <cstdint>
#include <optional>

namespace cpp_agent {
struct RequestMetrics {
    double duration_ms = 0;
    std::optional<double> first_text_ms;
    size_t input_content_bytes = 0;
    size_t tool_schema_bytes = 0;
    size_t output_content_bytes = 0;
    std::string status = "completed";
    json to_json() const {
        return {{"duration_ms", duration_ms},
                {"first_text_ms", first_text_ms ? json(*first_text_ms) : json(nullptr)},
                {"input_content_bytes", input_content_bytes}, {"tool_schema_bytes", tool_schema_bytes},
                {"output_content_bytes", output_content_bytes}, {"status", status}};
    }
};

struct ToolMetrics {
    std::string call_id;
    std::string name;
    double queue_ms = 0;
    double execution_ms = 0;
    size_t input_bytes = 0;
    size_t output_bytes = 0;
    bool success = false;
    json to_json() const {
        return {{"call_id", call_id}, {"name", name}, {"queue_ms", queue_ms},
                {"execution_ms", execution_ms}, {"input_bytes", input_bytes},
                {"output_bytes", output_bytes}, {"success", success}};
    }
};

struct RunMetrics {
    uint64_t run_id = 0;
    std::string status = "completed";
    double elapsed_ms = 0;
    double llm_ms = 0;
    double tools_wall_ms = 0;
    double tool_execution_ms = 0;
    double tool_queue_ms = 0;
    size_t llm_calls = 0;
    size_t tool_calls = 0;
    size_t input_bytes = 0;
    size_t output_bytes = 0;
    json to_json() const {
        return {{"run_id", run_id}, {"status", status}, {"elapsed_ms", elapsed_ms},
                {"llm_ms", llm_ms}, {"tools_wall_ms", tools_wall_ms},
                {"tool_execution_ms", tool_execution_ms}, {"tool_queue_ms", tool_queue_ms},
                {"llm_calls", llm_calls}, {"tool_calls", tool_calls},
                {"input_bytes", input_bytes}, {"output_bytes", output_bytes}};
    }
};
}
