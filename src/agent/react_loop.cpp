#include "agent/react_loop.h"
#include "memory/working_memory.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace cpp_agent {

// ============================================================================
// ReActLoop
// ============================================================================

namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
std::atomic<uint64_t> next_run_id{1};
size_t calls_bytes(const std::vector<ToolCall>& calls) {
    size_t bytes = 0;
    for (const auto& call : calls) bytes += call.function_name.size() + call.arguments.size();
    return bytes;
}
}

void ReActLoop::emit_trace(TraceStep step) {
    if (!trace_cb_) return;
    step.run_id = metrics_.run_id;
    step.elapsed_ms = milliseconds(run_started_);
    step.timestamp = std::chrono::system_clock::now();
    trace_cb_(step);
}

void ReActLoop::finish_metrics(const std::string& answer) noexcept {
    metrics_.elapsed_ms = milliseconds(run_started_);
    metrics_.output_bytes = answer.size();
    try {
        TraceStep step;
        step.phase = TraceStep::Phase::FINISH;
        step.iteration = last_iterations_ ? static_cast<int>(last_iterations_ - 1) : 0;
        step.llm_content = answer;
        step.run_metrics = metrics_;
        emit_trace(std::move(step));
    } catch (...) {
        // Observability must not mask a request exception or change its result.
    }
}

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
    run_started_ = Clock::now();
    metrics_ = {};
    metrics_.run_id = next_run_id.fetch_add(1, std::memory_order_relaxed);
    metrics_.input_bytes = user_input.size();
    try {
        auto answer = run_body(user_input, on_token, streaming);
        finish_metrics(answer);
        return answer;
    } catch (...) {
        metrics_.status = "error";
        finish_metrics("");
        throw;
    }
}

std::string ReActLoop::run_body(const std::string& user_input,
                               const std::function<void(const std::string&)>& on_token,
                               bool streaming) {
    auto stop = [&](const std::string& status, const std::string& message) {
        metrics_.status = status;
        return message;
    };
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

        if (history_.system_message() &&
            history_.system_message()->estimated_tokens() > config_.max_context_tokens)
            return stop("context_budget", "Agent stopped: system prompt exceeds context budget.");
        history_.truncate_to(config_.max_context_tokens);
        if (history_.messages().empty())
            return stop("context_budget", "Agent stopped: context budget cannot fit the conversation.");

        // Build messages vector from history
        std::vector<Message> msgs;
        msgs.reserve(history_.size() + (history_.system_message() ? 1 : 0));
        if (history_.system_message()) {
            msgs.push_back(*history_.system_message());
        }
        for (const auto& m : history_.messages()) {
            msgs.push_back(m);
        }

        json tools = build_tools_json();

        RequestMetrics request;
        for (const auto& msg : msgs)
            request.input_content_bytes += msg.content.size() + calls_bytes(msg.tool_calls);
        request.tool_schema_bytes = tools.dump().size();
        const auto request_started = Clock::now();
        size_t streamed_bytes = 0;
        auto measured_token = [&](const std::string& token) {
            if (!token.empty() && !request.first_text_ms)
                request.first_text_ms = milliseconds(request_started);
            streamed_bytes += token.size();
            if (on_token) on_token(token);
        };
        auto record_request = [&](const LLMResponse* response) {
            request.duration_ms = milliseconds(request_started);
            request.output_content_bytes = response
                ? response->content.size() + calls_bytes(response->tool_calls) : streamed_bytes;
            ++metrics_.llm_calls;
            metrics_.llm_ms += request.duration_ms;
        };
        auto record_failure = [&] {
            TraceStep failure;
            failure.iteration = static_cast<int>(iter);
            failure.phase = TraceStep::Phase::THINK;
            failure.detail = "LLM request " + request.status;
            failure.request_metrics = request;
            emit_trace(std::move(failure));
        };
        LLMResponse resp;
        try {
            llm_.set_request_timeout(config_.iteration_timeout_ms);
            resp = streaming ? llm_stream_once(msgs, tools, measured_token)
                             : llm_.chat_full(msgs, tools);
        } catch (const LLMTimeout&) {
            request.status = "timeout";
            record_request(nullptr);
            record_failure();
            return stop("timeout", "Agent stopped: LLM call exceeded iteration timeout.");
        } catch (...) {
            request.status = "error";
            record_request(nullptr);
            try { record_failure(); } catch (...) {}
            throw;
        }
        record_request(&resp);

        last_tokens_ += resp.total_tokens;

        // Trace: THINK
        if (trace_cb_) {
            TraceStep ts;
            ts.iteration = static_cast<int>(iter);
            ts.phase = TraceStep::Phase::THINK;
            ts.timestamp = std::chrono::system_clock::now();
            ts.llm_content = resp.content;
            ts.tool_calls = resp.tool_calls;
            ts.request_metrics = request;
            ts.llm_response = resp;  // Day 16: capture full LLM response
            ts.detail = resp.has_tool_calls
                ? fmt::format("LLM wants to call {} tool(s)", resp.tool_calls.size())
                : "LLM produced final answer";
            emit_trace(std::move(ts));
        }

        // ── No tool calls → this is the final answer ───────────────────────
        // A token cap can interrupt both text and tool arguments. Never execute
        // an incomplete tool call or report a truncated answer as completed.
        if (resp.finish_reason == "length") {
            metrics_.status = "output_limit";
            return resp.content;
        }
        if (resp.finish_reason == "content_filter") {
            metrics_.status = "content_filter";
            return resp.content;
        }
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
        if (auto reason = detect_loop(resp.tool_calls); reason.has_value()) {
            spdlog::warn("ReAct: {}", *reason);
            return stop("loop_detected", *reason);
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
            emit_trace(std::move(ts));
        }

        // ── OBSERVE ─────────────────────────────────────────────────────────
        std::vector<ToolResult> trace_results;
        std::vector<ToolMetrics> tool_metrics;
        const auto batch_started = Clock::now();
        std::vector<Message> tool_msgs = execute_tools(resp.tool_calls,
            trace_cb_ ? &trace_results : nullptr, &tool_metrics);
        double batch_ms = milliseconds(batch_started);
        metrics_.tools_wall_ms += batch_ms;
        for (const auto& metric : tool_metrics) {
            ++metrics_.tool_calls;
            metrics_.tool_execution_ms += metric.execution_ms;
            metrics_.tool_queue_ms += metric.queue_ms;
        }
        for (auto& tm : tool_msgs) {
            history_.push(std::move(tm));
        }

        if (trace_cb_) {
            TraceStep ts;
            ts.iteration = static_cast<int>(iter);
            ts.phase = TraceStep::Phase::OBSERVE;
            ts.timestamp = std::chrono::system_clock::now();
            ts.tool_metrics = std::move(tool_metrics);
            ts.tools_wall_ms = batch_ms;
            ts.tool_results = std::move(trace_results);  // Day 16: capture actual results
            ts.detail = fmt::format("{} tool result(s) appended", ts.tool_results.size());
            emit_trace(std::move(ts));
        }

        // ── Truncate if over token budget ───────────────────────────────────
        size_t truncated = history_.truncate_to(config_.max_context_tokens);
        if (truncated > 0) {
            spdlog::warn("ReAct: truncated {} messages (token budget exceeded)", truncated);
        }
    }

    return stop("iteration_limit", "Agent stopped: exceeded iteration limit without a final answer.");
}

LLMResponse ReActLoop::llm_stream_once(
    const std::vector<Message>& msgs,
    const json& tools,
    const std::function<void(const std::string&)>& on_token) {
    // Block the loop until the streaming call signals completion. The callback-
    // based chat_full_streaming API hands us the final LLMResponse in on_complete.
    auto promise = std::make_shared<std::promise<LLMResponse>>();
    auto future = promise->get_future();

    llm_.chat_full_streaming(msgs, tools, on_token,
        [promise](LLMResponse resp) { promise->set_value(std::move(resp)); });

    return future.get();
}

std::optional<std::string> ReActLoop::detect_loop(const std::vector<ToolCall>& calls) {
    // Fingerprint = function_name:arguments for each call, concatenated. The
    // call id is deliberately excluded — a model stuck in a loop regenerates
    // fresh ids while repeating the exact same request.
    std::string fp;
    fp.reserve(calls_bytes(calls) + calls.size() * 2);
    for (const auto& tc : calls) {
        fp.append(tc.function_name).push_back(':');
        fp.append(tc.arguments).push_back(';');
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
    std::vector<ToolResult>* out_trace_results, std::vector<ToolMetrics>* out_metrics) {
    std::vector<ToolResult> results(calls.size());
    std::vector<ToolMetrics> timing(calls.size());
    // Every call in this batch is eligible at the same instant. In serial
    // mode, later calls therefore include time waiting for earlier calls.
    const auto eligible = Clock::now();
    auto execute = [&](size_t i) {
        const auto& call = calls[i];
        auto& metric = timing[i];
        metric.call_id = call.id;
        metric.name = call.function_name;
        metric.input_bytes = call.arguments.size();
        const auto started = Clock::now();
        metric.queue_ms = std::chrono::duration<double, std::milli>(started - eligible).count();
        json args;
        if (auto parsed = call.parsed_arguments()) args = std::move(*parsed);
        results[i] = registry_.execute(call.function_name, args, call.id);
        metric.execution_ms = milliseconds(started);
        metric.output_bytes = results[i].content.size();
        metric.success = results[i].success;
    };
    if (parallel_tools_ && calls.size() > 1) {
        std::vector<std::future<void>> futures;
        futures.reserve(calls.size());
        for (size_t i = 0; i < calls.size(); ++i)
            futures.push_back(std::async(std::launch::async, execute, i));
        for (auto& future : futures) future.get();
    } else {
        for (size_t i = 0; i < calls.size(); ++i) execute(i);
    }
    std::vector<Message> messages;
    messages.reserve(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
        // Only the trace subscriber needs a second copy of tool output.
        messages.push_back(Message::tool_result(calls[i].id, calls[i].function_name,
            out_trace_results ? results[i].content : std::move(results[i].content)));
    }
    if (out_trace_results) *out_trace_results = std::move(results);
    if (out_metrics) *out_metrics = std::move(timing);
    return messages;
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
    j["run_id"] = run_id;
    if (elapsed_ms) j["elapsed_ms"] = *elapsed_ms;
    if (request_metrics) j["request_metrics"] = request_metrics->to_json();
    if (run_metrics) j["run_metrics"] = run_metrics->to_json();
    if (tools_wall_ms) j["tools_wall_ms"] = *tools_wall_ms;
    if (request_metrics) j["duration_ms"] = request_metrics->duration_ms;
    else if (tools_wall_ms) j["duration_ms"] = *tools_wall_ms;
    else if (run_id) j["duration_ms"] = 0;
    if (!tool_metrics.empty()) {
        j["tool_metrics"] = json::array();
        for (const auto& metric : tool_metrics) j["tool_metrics"].push_back(metric.to_json());
    }
    j["phase"] = phase == Phase::THINK ? "THINK"
               : phase == Phase::ACT ? "ACT"
               : phase == Phase::OBSERVE ? "OBSERVE" : "FINISH";
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
