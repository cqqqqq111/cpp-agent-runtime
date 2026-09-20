/**
 * @file py_bridge.h
 * @brief Public API for the C++ ↔ Python bridge.
 *
 * These free functions live in the global namespace and are implemented
 * in py_bridge.cpp via pybind11. Include this header from any C++ file
 * that needs to call into Python.
 *
 * Design note: keeping these as free functions (not in cpp_agent namespace)
 * simplifies pybind11's embedded interpreter management. The rest of the
 * project's types live in cpp_agent::.
 */

#pragma once

#include <functional>
#include <string>

/// Start the embedded Python interpreter.
/// Must be called once before any LLM calls. Returns 0 on success.
int init_python_bridge();

/// Shut down the embedded Python interpreter.
/// Safe to call multiple times. No-op if not initialized.
void shutdown_python_bridge();

/// Simple single-turn LLM call through the Python bridge.
/// @throws std::runtime_error if the bridge is not initialized or Python fails.
std::string call_llm_bridge(const std::string& prompt);

/// Streaming variant that accumulates all tokens and returns the full response.
/// @throws std::runtime_error if the bridge is not initialized or Python fails.
std::string call_llm_streaming_bridge(const std::string& prompt);

/// Day 4+: Full multi-turn chat with tools.
/// @returns JSON string of the LLMResponse.
std::string call_llm_messages_bridge(const std::string& messages_json,
                                     const std::string& tools_json);

/// Day 9: Embed text to a vector via Python/OpenAI.
std::string call_embedding_bridge(const std::string& text);

/// Day 11: Streaming multi-turn LLM call with token callback.
/// @param on_token  Called for each text token as it arrives.
/// @param on_complete  Called with the full JSON response when streaming ends.
void call_llm_streaming_bridge(
    const std::string& messages_json,
    const std::string& tools_json,
    const std::function<void(const std::string&)>& on_token,
    const std::function<void(const std::string&)>& on_complete);

/// Day 17: Report whether an LLM is configured and the active model.
/// The .env file is loaded on the Python side, so C++ asks Python instead of
/// reading process env vars directly.
/// @returns JSON string {"configured": bool, "model": str}.
std::string llm_config_bridge();
