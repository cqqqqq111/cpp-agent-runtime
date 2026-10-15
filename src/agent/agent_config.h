/**
 * @file agent_config.h
 * @brief Configuration for an Agent instance.
 */

#pragma once

#include <cstddef>
#include <string>

namespace cpp_agent {

struct AgentConfig {
    /// Natural-language system prompt that defines the agent's role.
    std::string system_prompt = "You are a helpful AI assistant.";

    /// Maximum iterations of the ReAct loop (safety limit).
    /// Prevents infinite tool-calling loops.
    size_t max_iterations = 10;

    /// Maximum total tokens before truncation kicks in.
    size_t max_context_tokens = 8000;

    /// LLM model name.
    std::string model = "gpt-4o-mini";

    /// Temperature for LLM sampling.
    float temperature = 0.2f;

    /// Day 22: per-iteration timeout for a single LLM call (milliseconds).
    /// Enforced by the LLM adapter/transport; unsupported adapters reject it.
    /// 0 disables the total deadline (transport IO timeouts may still apply).
    int iteration_timeout_ms = 0;
};

}  // namespace cpp_agent
