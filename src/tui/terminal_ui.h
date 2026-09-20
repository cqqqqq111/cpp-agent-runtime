/**
 * @file terminal_ui.h
 * @brief Lightweight ANSI terminal UI for cpp-agent-runtime.
 *
 * No external dependencies — pure ANSI escape codes. Three-panel layout:
 *   Top:    Status bar    (token count, iterations, agent state)
 *   Middle: Conversation  (color-coded THINK/ACT/OBSERVE/RESULT)
 *   Bottom: Input prompt  (readline-style)
 *
 * Architecture:
 *   Main thread  → render loop + input polling
 *   Agent thread → ReActLoop::run() → trace callbacks → shared log buffer
 */

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cpp_agent {

struct LogEntry {
    enum class Kind { SYSTEM, USER, THINK, ACT, OBSERVE, RESULT, ERROR };
    Kind kind = Kind::SYSTEM;
    std::string text;
};

class TerminalUI {
public:
    TerminalUI();
    ~TerminalUI();

    /// Start interactive loop. on_submit(input) → agent_response (blocks).
    void run(std::function<std::string(const std::string&)> on_submit);

    /// Thread-safe log.
    void log(LogEntry::Kind kind, const std::string& text);

    /// Day 18: append text to the last log entry of the same kind (typewriter
    /// effect for streaming). If the last entry is a different kind, this
    /// pushes a new entry instead.
    void append_to_last(LogEntry::Kind kind, const std::string& text);

    /// Thread-safe status update.
    void set_status(const std::string& text);
    void set_stats(int tokens, int iterations, double elapsed_ms);

private:
    std::mutex mutex_;
    std::deque<LogEntry> log_;
    std::string status_ = "Ready";
    int tokens_ = 0, iterations_ = 0;
    double elapsed_ms_ = 0;
    std::atomic<bool> agent_busy_{false};

    // Day 19: how many entries the user has scrolled back from the newest
    // (0 = pinned to the bottom; grows as the user scrolls up).
    int scroll_offset_ = 0;

    // Day 17: track the agent thread so we can join it on exit instead of
    // detaching and risking a dangling `this` pointer.
    std::atomic<bool> cancel_{false};
    std::jthread agent_thread_;

    // ANSI helpers
    static const char* ansi_color(LogEntry::Kind k);
    void render_frame(const std::string& input_line) const;
    void draw_box(int y, int x, int h, int w) const;
};

}  // namespace cpp_agent
