#include "tracing/trace_recorder.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cpp_agent {

// ============================================================================
// Helpers
// ============================================================================

namespace {

/// Convert system_clock::time_point to ISO 8601 string (same helper as react_loop.cpp).
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

/// Count tool calls from all steps.
size_t count_tool_calls(const std::vector<TraceStep>& steps) {
    size_t n = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::ACT) {
            n += s.tool_calls.size();
        }
    }
    return n;
}

/// Count tool successes/failures from OBSERVE steps.
void count_tool_outcomes(const std::vector<TraceStep>& steps,
                          size_t& successes, size_t& failures) {
    successes = 0;
    failures = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::OBSERVE) {
            for (const auto& tr : s.tool_results) {
                if (tr.success) ++successes;
                else ++failures;
            }
        }
    }
}

/// Count LLM calls (THINK steps).
size_t count_llm_calls(const std::vector<TraceStep>& steps) {
    size_t n = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::THINK) ++n;
    }
    return n;
}

/// Sum token totals from THINK steps (from llm_response).
size_t sum_tokens(const std::vector<TraceStep>& steps) {
    size_t tokens = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::THINK && s.llm_response.has_value()) {
            tokens += s.llm_response->total_tokens;
        }
    }
    return tokens;
}

/// Sum prompt tokens.
size_t sum_prompt_tokens(const std::vector<TraceStep>& steps) {
    size_t tokens = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::THINK && s.llm_response.has_value()) {
            tokens += s.llm_response->prompt_tokens;
        }
    }
    return tokens;
}

/// Sum completion tokens.
size_t sum_completion_tokens(const std::vector<TraceStep>& steps) {
    size_t tokens = 0;
    for (const auto& s : steps) {
        if (s.phase == TraceStep::Phase::THINK && s.llm_response.has_value()) {
            tokens += s.llm_response->completion_tokens;
        }
    }
    return tokens;
}

}  // anonymous namespace

// ============================================================================
// TraceRecorder
// ============================================================================

TraceRecorder::TraceRecorder(std::string output_path)
    : output_path_(std::move(output_path))
    , start_time_(std::chrono::system_clock::now()) {
    if (output_path_.empty()) {
        output_path_ = generate_filename();
    }
    spdlog::info("TraceRecorder: output = {}", output_path_);
}

void TraceRecorder::record(const TraceStep& step) {
    if (step.run_id && step.run_id != active_run_id_) {
        // One trace is one run, even when the recorder is reused by the TUI.
        steps_.clear();
        metrics_.reset();
        finalized_ = false;
        final_answer_.clear();
        elapsed_ms_ = 0;
        active_run_id_ = step.run_id;
    }
    if (step.phase == TraceStep::Phase::FINISH) {
        metrics_ = step.run_metrics;
        if (metrics_) {
            elapsed_ms_ = metrics_->elapsed_ms;
            final_answer_ = step.llm_content;
            finalized_ = true;
        }
        return;
    }
    steps_.push_back(step);
}

ReActLoop::TraceCallback TraceRecorder::wrap(ReActLoop::TraceCallback existing) {
    // Capture `this` — TraceRecorder must outlive the callback.
    // In practice, TraceRecorder lives on main()'s stack and the callback
    // is invoked during agent.run() which is called within main()'s scope.
    return [this, cb = std::move(existing)](const TraceStep& step) {
        this->record(step);
        if (step.phase == TraceStep::Phase::FINISH) {
            // Preserve diagnostic traces even when run() rethrows an exception
            // and the caller never reaches its normal save_to_file().
            if (step.run_metrics && step.run_metrics->status == "error") save_to_file();
            return;
        }
        if (cb) cb(step);
    };
}

void TraceRecorder::set_config(const AgentConfig& config) {
    config_ = config;
}

void TraceRecorder::set_query(const std::string& query) {
    query_ = query;
    start_time_ = std::chrono::system_clock::now();
}

void TraceRecorder::finalize(size_t total_llm_calls, size_t total_tokens,
                              double elapsed_ms, const std::string& final_answer) {
    total_llm_calls_ = total_llm_calls;
    total_tokens_ = total_tokens;
    elapsed_ms_ = elapsed_ms;
    final_answer_ = final_answer;
    finalized_ = true;
    spdlog::info("TraceRecorder: finalized — {} steps, {} LLM calls, {} tokens, {:.1f}ms",
                 steps_.size(), total_llm_calls_, total_tokens_, elapsed_ms_);
}

json TraceRecorder::to_json() const {
    json j;

    j["version"] = "1.1.0";
    j["generated_by"] = "cpp-agent-runtime 0.1.0";
    j["exported_at"] = time_point_to_iso(std::chrono::system_clock::now());

    // Config
    j["config"] = {
        {"system_prompt", config_.system_prompt},
        {"model", config_.model},
        {"max_iterations", config_.max_iterations},
        {"max_context_tokens", config_.max_context_tokens},
        {"temperature", config_.temperature}
    };

    // Query
    j["query"] = {
        {"user_input", query_},
        {"timestamp", time_point_to_iso(start_time_)}
    };

    // Summary (compute from steps for accuracy)
    size_t tool_calls = count_tool_calls(steps_);
    size_t successes = 0, failures = 0;
    count_tool_outcomes(steps_, successes, failures);

    j["summary"] = {
        {"total_iterations", steps_.empty() ? 0 : steps_.back().iteration + 1},
        {"total_steps", steps_.size()},
        {"total_llm_calls", count_llm_calls(steps_)},
        {"total_tokens", sum_tokens(steps_)},
        {"total_prompt_tokens", sum_prompt_tokens(steps_)},
        {"total_completion_tokens", sum_completion_tokens(steps_)},
        {"total_tool_calls", tool_calls},
        {"total_tool_successes", successes},
        {"total_tool_failures", failures},
        {"elapsed_ms", elapsed_ms_},
        {"final_answer", final_answer_}
    };
    if (metrics_) {
        j["summary"]["performance"] = metrics_->to_json();
        j["summary"]["elapsed_ms"] = metrics_->elapsed_ms;
    }

    // Steps with per-step elapsed_ms
    json steps_arr = json::array();
    if (!steps_.empty()) {
        auto first_ts = steps_.front().timestamp;
        for (size_t i = 0; i < steps_.size(); ++i) {
            json sj = steps_[i].to_json();
            sj["index"] = i;
            auto elapsed = std::chrono::duration<double, std::milli>(
                steps_[i].timestamp - first_ts).count();
            sj["elapsed_ms"] = steps_[i].elapsed_ms.value_or(elapsed);
            steps_arr.push_back(sj);
        }
    }
    j["steps"] = steps_arr;

    return j;
}

bool TraceRecorder::save_to_file() const {
    ensure_logs_dir();

    std::ofstream file(output_path_);
    if (!file) {
        spdlog::error("TraceRecorder: cannot open file for writing: {}", output_path_);
        return false;
    }

    file << to_json().dump(2);
    auto file_size = static_cast<long>(file.tellp());
    spdlog::info("TraceRecorder: trace saved to {} ({} bytes)",
                 output_path_, file_size);
    return true;
}

std::string TraceRecorder::generate_filename() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch() % std::chrono::seconds(1)).count();

    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "logs/trace_%Y%m%d_%H%M%S", &tm_buf);
    return fmt::format("{}_{:06d}.json", buf, us);
}

void TraceRecorder::ensure_logs_dir() {
    std::error_code ec;
    std::filesystem::create_directories("logs", ec);
    if (ec) {
        spdlog::warn("TraceRecorder: could not create logs/ directory: {}", ec.message());
    }
}

}  // namespace cpp_agent
