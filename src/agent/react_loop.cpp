#include "agent/react_loop.h"
#include "memory/working_memory.h"

#include <spdlog/spdlog.h>

#include <future>
#include <string>
#include <thread>
#include <vector>

namespace cpp_agent {

// ============================================================================
// ReActLoop
// ============================================================================

ReActLoop::ReActLoop(LLMAdapter& llm, ToolRegistry& registry, const AgentConfig& config)
    : llm_(llm), registry_(registry), config_(config) {}

std::string ReActLoop::run(const std::string& user_input) {
    return run_impl(user_input, {}, /*streaming=*/false);
}

std::string ReActLoop::run_streaming(const std::string& user_input,
                                     std::function<void(const std::string&)> on_token) {
    return run_impl(user_input, std::move(on_token), /*streaming=*/true);
}

std::string ReActLoop::run_impl(const std::string& user_input,
                                const std::function<void(const std::string&)>& on_token,
                                bool streaming) {
    last_iterations_ = 0;
    last_tokens_ = 0;
    last_fingerprint_.clear();
    recent_fingerprints_.clear();

    // Reset conversation: set system prompt (with working memory if available).
    history_.clear();
    std::string effective_system = config_.system_prompt;
    if (working_memory_) {
        std::string wm_ctx = working_memory_->to_context_string();
        if (!wm_ctx.empty()) {
            effective_system = wm_ctx + "\n\n" + effective_system;
            spdlog::debug("ReAct: injected working memory ({} active entries)",
                          working_memory_->active_entries().size());
        }
    }
    history_.set_system_message(Message::system(effective_system));
    history_.push(Message::user(user_input));

    spdlog::info("ReAct: starting loop (max_iter={}, streaming={})",
                 config_.max_iterations, streaming);

    // ---- Main THINK → ACT → OBSERVE loop -----------------------------------
    for (size_t iter = 0; iter < config_.max_iterations; ++iter) {
        last_iterations_ = iter + 1;

        // ── THINK ───────────────────────────────────────────────────────────
        spdlog::debug("ReAct iter {}: THINK ({} messages, ~{} tokens)",
                      iter, history_.size(), history_.estimated_tokens());

        // Build messages vector from history
        std::vector<Message> msgs;
        if (history_.system_message()) {
            msgs.push_back(*history_.system_message());
        }
        for (const auto& m : history_.messages()) {
            msgs.push_back(m);
        }

        json tools = build_tools_json();

        // Day 18: streaming uses the callback-based API; non-streaming uses
        // the plain blocking call.
        //
        // Day 22: optional per-iteration timeout. The (blocking) LLM call runs
        // on a worker thread and we wait up to iteration_timeout_ms. On timeout
        // we detach the worker — it owns copies of its inputs, so it can finish
        // on its own — and abort gracefully. Limitation: a blocked call can't be
        // truly cancelled; the detached thread keeps running until the client
        // returns. Real cancellation needs LLM-client-side support.
        LLMResponse resp;
        if (config_.iteration_timeout_ms > 0) {
            std::promise<LLMResponse> promise;
            auto future = promise.get_future();

            std::vector<Message> msgs_copy = msgs;
            json tools_copy = tools;
            auto on_token_copy = on_token;
            bool do_stream = streaming;

            std::thread worker(
                [this, msgs_copy = std::move(msgs_copy),
                 tools_copy = std::move(tools_copy),
                 on_token_copy = std::move(on_token_copy), do_stream,
                 promise = std::move(promise)]() mutable {
                    try {
                        LLMResponse r = do_stream
                            ? llm_stream_once(msgs_copy, tools_copy, on_token_copy)
                            : llm_.chat_full(msgs_copy, tools_copy);
                        promise.set_value(std::move(r));
                    } catch (...) {
                        promise.set_exception(std::current_exception());
                    }
                });

            if (future.wait_for(std::chrono::milliseconds(config_.iteration_timeout_ms))
                    == std::future_status::timeout) {
                spdlog::warn("ReAct: LLM call exceeded {}ms iteration timeout",
                             config_.iteration_timeout_ms);
                worker.detach();
                return "Agent stopped: LLM call exceeded iteration timeout.";
            }
            worker.join();
            resp = future.get();
        } else {
            resp = streaming ? llm_stream_once(msgs, tools, on_token)
                             : llm_.chat_full(msgs, tools);
        }

        last_tokens_ += resp.total_tokens;

        // Trace: THINK
        if (trace_cb_) {
            TraceStep ts;
            ts.iteration = static_cast<int>(iter);
            ts.phase = TraceStep::Phase::THINK;
            ts.timestamp = std::chrono::system_clock::now();
            ts.llm_content = resp.content;
            ts.tool_calls = resp.tool_calls;
            ts.llm_response = resp;  // Day 16: capture full LLM response
            ts.detail = resp.has_tool_calls
                ? fmt::format("LLM wants to call {} tool(s)", resp.tool_calls.size())
                : "LLM produced final answer";
            trace_cb_(ts);
        }

        // ── No tool calls → this is the final answer ───────────────────────
        if (!resp.has_tool_calls) {
            std::string answer = resp.content;
            if (!answer.empty()) {
                history_.push(Message::assistant(answer));
            }
            spdlog::info("ReAct: finished after {} iterations ({} tokens)",
                         last_iterations_, last_tokens_);
            return answer;
        }

        // ── Day 18 Task 1: loop detection ──────────────────────────────────
        if (auto stop = detect_loop(resp.tool_calls); stop.has_value()) {
            spdlog::warn("ReAct: {}", *stop);
            return *stop;
        }

        // ── ACT ─────────────────────────────────────────────────────────────
        // LLM wants tools → create assistant message with tool_calls
        history_.push(Message::assistant_tool_calls(resp.tool_calls));

        if (trace_cb_) {
            TraceStep ts;
            ts.iteration = static_cast<int>(iter);
            ts.phase = TraceStep::Phase::ACT;
            ts.timestamp = std::chrono::system_clock::now();
            ts.tool_calls = resp.tool_calls;
            ts.detail = fmt::format("Executing {} tool(s)", resp.tool_calls.size());
            trace_cb_(ts);
        }

        // ── OBSERVE ─────────────────────────────────────────────────────────
        std::vector<ToolResult> trace_results;
        std::vector<Message> tool_msgs = execute_tools(resp.tool_calls, &trace_results);
        for (auto& tm : tool_msgs) {
            history_.push(std::move(tm));
        }

        if (trace_cb_) {
            TraceStep ts;
            ts.iteration = static_cast<int>(iter);
            ts.phase = TraceStep::Phase::OBSERVE;
            ts.timestamp = std::chrono::system_clock::now();
            ts.tool_results = std::move(trace_results);  // Day 16: capture actual results
            ts.detail = fmt::format("{} tool result(s) appended", ts.tool_results.size());
            trace_cb_(ts);
        }

        // ── Truncate if over token budget ───────────────────────────────────
        size_t truncated = history_.truncate_to(config_.max_context_tokens);
        if (truncated > 0) {
            spdlog::warn("ReAct: truncated {} messages (token budget exceeded)", truncated);
        }
    }

    // Max iterations reached — return whatever the last LLM response said.
    spdlog::warn("ReAct: max iterations ({}) reached, returning last response.",
                 config_.max_iterations);
    std::string last_content = history_.messages().empty()
        ? "(agent exceeded iteration limit)"
        : history_.messages().back().content;

    if (last_content.empty()) {
        last_content = "(agent produced no final answer within iteration limit)";
    }
    return last_content;
}

LLMResponse ReActLoop::llm_stream_once(
    const std::vector<Message>& msgs,
    const json& tools,
    const std::function<void(const std::string&)>& on_token) {
    // Block the loop until the streaming call signals completion. The callback-
    // based chat_full_streaming API hands us the final LLMResponse in on_complete.
    std::promise<LLMResponse> promise;
    auto future = promise.get_future();

    llm_.chat_full_streaming(msgs, tools, on_token,
        [&promise](LLMResponse resp) { promise.set_value(std::move(resp)); });

    return future.get();
}

std::optional<std::string> ReActLoop::detect_loop(const std::vector<ToolCall>& calls) {
    // Fingerprint = function_name:arguments for each call, concatenated. The
    // call id is deliberately excluded — a model stuck in a loop regenerates
    // fresh ids while repeating the exact same request.
    std::string fp;
    for (const auto& tc : calls) {
        fp += tc.function_name + ":" + tc.arguments + ";";
    }
    if (fp.empty()) return std::nullopt;

    // Exact repeat: same request as the immediately previous iteration.
    if (fp == last_fingerprint_) {
        return "Agent stopped: detected repeated tool call (loop prevention).";
    }

    // ABAB oscillation: the last three requests were A → B → A, and this one
    // is B again → the agent is flip-flopping between two calls.
    if (recent_fingerprints_.size() >= 3) {
        const auto& a = recent_fingerprints_[recent_fingerprints_.size() - 3];
        const auto& b = recent_fingerprints_[recent_fingerprints_.size() - 2];
        const auto& c = recent_fingerprints_[recent_fingerprints_.size() - 1];
        if (a == c && a != b && b == fp) {
            return "Agent stopped: detected repeated tool-call pattern (loop prevention).";
        }
    }

    last_fingerprint_ = fp;
    recent_fingerprints_.push_back(fp);
    if (recent_fingerprints_.size() > 3) {
        recent_fingerprints_.erase(recent_fingerprints_.begin());
    }
    return std::nullopt;
}

// ============================================================================
// Private helpers
// ============================================================================

std::vector<Message> ReActLoop::execute_tools(const std::vector<ToolCall>& calls,
                                                std::vector<ToolResult>* out_trace_results) {
    // ── Parallel path: dispatch each (blocking) tool call onto its own worker
    // thread so they overlap in wall-clock time. The Day 11 coroutine version
    // only interleaved them on a single thread (no speedup); real parallelism
    // requires threads because tools' execute() is synchronous and never yields.
    if (parallel_tools_ && calls.size() > 1) {
        spdlog::info("ReAct: executing {} tools in parallel", calls.size());

        std::vector<ToolResult> tr_results(calls.size());
        std::vector<std::future<void>> futures;
        futures.reserve(calls.size());

        for (size_t i = 0; i < calls.size(); ++i) {
            futures.push_back(std::async(std::launch::async, [this, &calls, &tr_results, i] {
                const auto& call = calls[i];

                json args;
                auto parsed = call.parsed_arguments();
                if (parsed) args = *parsed;

                // Each worker writes its own slot → no data race on the vector.
                tr_results[i] = registry_.execute(call.function_name, args, call.id);
            }));
        }

        // Join in call order; also guarantees tr_results is fully populated
        // before the assembly loop below reads it.
        for (auto& f : futures) f.get();

        std::vector<Message> out;
        out.reserve(calls.size());
        for (size_t i = 0; i < calls.size(); ++i) {
            const auto& tr = tr_results[i];
            spdlog::debug("ReAct: tool '{}' (parallel) → success={}",
                          calls[i].function_name, tr.success);
            out.push_back(Message::tool_result(calls[i].id, calls[i].function_name, tr.content));
        }

        // Feed trace results back
        if (out_trace_results) {
            out_trace_results->reserve(tr_results.size());
            for (auto& tr : tr_results) out_trace_results->push_back(std::move(tr));
        }

        return out;
    }

    // ── Sequential fallback ───────────────────────────────────────────────
    std::vector<Message> results;
    results.reserve(calls.size());

    for (const auto& call : calls) {
        spdlog::info("ReAct: executing tool '{}' (id={})", call.function_name, call.id);

        json args;
        auto parsed = call.parsed_arguments();
        if (parsed) args = *parsed;

        ToolResult tr = registry_.execute(call.function_name, args, call.id);
        if (out_trace_results) {
            out_trace_results->push_back(tr);  // Day 16: capture for tracing
        }
        results.push_back(Message::tool_result(call.id, call.function_name, tr.content));

        spdlog::debug("ReAct: tool '{}' → success={}, output_len={}",
                      call.function_name, tr.success, tr.content.size());
    }

    return results;
}

json ReActLoop::build_tools_json() {
    return registry_.to_openai_tools();
}

// ============================================================================
// TraceStep JSON serialization (Day 16: trace visualization)
// ============================================================================

namespace {

/// Convert system_clock::time_point to ISO 8601 string with microseconds.
std::string time_point_to_iso(std::chrono::system_clock::time_point tp) {
    auto t = std::chrono::system_clock::to_time_t(tp);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        tp.time_since_epoch() % std::chrono::seconds(1)).count();

    std::tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return fmt::format("{}.{:06d}Z", buf, us);
}

}  // anonymous namespace

json TraceStep::to_json() const {
    json j;
    j["iteration"] = iteration;
    j["phase"] = phase == Phase::THINK ? "THINK"
               : phase == Phase::ACT ? "ACT"
               : "OBSERVE";
    j["timestamp"] = time_point_to_iso(timestamp);
    j["detail"] = detail;
    j["llm_content"] = llm_content;

    if (llm_response.has_value()) {
        j["llm_response"] = llm_response->to_json();
    } else {
        j["llm_response"] = nullptr;
    }

    json tcs = json::array();
    for (const auto& tc : tool_calls) {
        tcs.push_back(tc.to_json());
    }
    j["tool_calls"] = tcs;

    json trs = json::array();
    for (const auto& tr : tool_results) {
        trs.push_back(tr.to_json());
    }
    j["tool_results"] = trs;

    return j;
}

}  // namespace cpp_agent
