/**
 * @file trace_recorder.h
 * @brief TraceRecorder — collects ReAct loop trace steps and exports as JSON.
 *
 * Day 16: Reasoning chain visualization.
 *
 * The TraceRecorder wraps the existing trace_callback mechanism, intercepting
 * every TraceStep event and storing it. After the agent run completes,
 * finalize() computes summary stats and save_to_file() writes a pretty-printed
 * JSON file suitable for the web/index.html visualization panel.
 *
 * Usage:
 *   TraceRecorder recorder("logs/demo.json");
 *   agent.set_trace_callback(recorder.wrap(existing_tui_callback));
 *   agent.run("user query");
 *   recorder.finalize(iterations, tokens, elapsed_ms, answer);
 *   recorder.save_to_file();
 */

#pragma once

#include "agent/agent_config.h"
#include "agent/react_loop.h"

#include <chrono>
#include <string>
#include <vector>

namespace cpp_agent {

class TraceRecorder {
public:
    /// @param output_path  Path to save the trace JSON file.
    ///                     If empty, auto-generates "logs/trace_<timestamp>.json".
    explicit TraceRecorder(std::string output_path = "");

    /// Record a single trace step (called from trace_callback).
    void record(const TraceStep& step);

    /// Return a TraceCallback that records steps AND forwards to `existing`.
    /// This is the non-intrusive integration point — TUI callback keeps working.
    ReActLoop::TraceCallback wrap(ReActLoop::TraceCallback existing);

    /// Set agent configuration for the trace header metadata.
    void set_config(const AgentConfig& config);

    /// Set the user query that triggered this agent run.
    void set_query(const std::string& query);

    /// Mark the run as complete with aggregate statistics.
    /// @param total_llm_calls  Number of LLM API calls made.
    /// @param total_tokens     Total tokens consumed.
    /// @param elapsed_ms       Wall-clock elapsed time in milliseconds.
    /// @param final_answer     The agent's final text response.
    void finalize(size_t total_llm_calls, size_t total_tokens,
                  double elapsed_ms, const std::string& final_answer);

    /// Serialize the complete trace to a JSON object.
    json to_json() const;

    /// Save the trace to the output file. Creates directories if needed.
    /// @returns true on success, false on filesystem error.
    bool save_to_file() const;

    /// Number of steps recorded so far.
    size_t step_count() const noexcept { return steps_.size(); }

    /// Has finalize() been called?
    bool is_finalized() const noexcept { return finalized_; }

private:
    std::vector<TraceStep> steps_;
    std::string output_path_;
    AgentConfig config_;
    std::string query_;
    std::string final_answer_;
    size_t total_llm_calls_ = 0;
    size_t total_tokens_ = 0;
    double elapsed_ms_ = 0.0;
    std::chrono::system_clock::time_point start_time_;
    bool finalized_ = false;
    uint64_t active_run_id_ = 0;
    std::optional<RunMetrics> metrics_;

    /// Generate a timestamp-based filename if none was provided.
    static std::string generate_filename();

    /// Ensure the logs/ directory exists.
    static void ensure_logs_dir();
};

}  // namespace cpp_agent
