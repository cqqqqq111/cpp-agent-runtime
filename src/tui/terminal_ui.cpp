/**
 * @file terminal_ui.cpp
 * @brief Pure ANSI terminal UI — no external dependencies.
 *
 * Uses ANSI escape codes (\033[...m for colors, \033[y;xH for cursor) to
 * render a three-panel agent interface. Runs a simple polled-input loop:
 * each frame, check for console input, render, sleep briefly.
 *
 * Tested on: Windows Terminal, cmd.exe (Win10+), xterm, iTerm2.
 */

#include "tui/terminal_ui.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <conio.h>   // _kbhit, _getch
#include <windows.h> // SetConsoleMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING
// windows.h #defines ERROR as 0 — conflicts with our enum
#undef ERROR
#else
#include <sys/ioctl.h>  // TIOCGWINSZ (terminal size)
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cpp_agent {

// ============================================================================
// Platform: enable ANSI on Windows
// ============================================================================
static void enable_ansi() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);

    // Chinese Windows defaults to GBK (CP936); our source files are UTF-8.
    // Switch the console to UTF-8 so box-drawing characters render correctly
    // instead of mojibake (鈹屸攢...).
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// Repeat a (possibly multi-byte UTF-8) string `n` times.
// NOTE: std::string(n, '─') would truncate the 3-byte '─' down to a single
// char (compiler warns: overflow 14849152 -> -128), which is the root cause
// of the horizontal-line mojibake. This helper repeats the full UTF-8 bytes.
static std::string repeat_str(const std::string& s, int n) {
    std::string out;
    out.reserve(s.size() * static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

// ============================================================================
// Platform: terminal size (Day 19 — dynamic dimensions)
// ============================================================================

/// Max entries kept in the scroll buffer (Day 19: 50 → 500).
constexpr size_t kMaxLogSize = 500;

/// Fixed vertical layout budget (rows NOT used by the conversation area).
constexpr int kStatusH = 3;
constexpr int kInputH = 3;

/// Query the current terminal width/height. Falls back to 80×24 if the size
/// can't be determined.
static void get_terminal_size(int& w, int& h) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        w = ws.ws_col;
        h = ws.ws_row;
        return;
    }
#endif
    w = 80;
    h = 24;  // conservative fallback
}

/// Height available for the conversation area, given the current terminal.
static int conversation_height() {
    int w = 0, h = 0;
    get_terminal_size(w, h);
    // status (3) + input (3) + one blank separator row are fixed overhead.
    return std::max(3, h - kStatusH - kInputH - 1);
}

// ============================================================================
// Platform: non-blocking key check
// ============================================================================
static bool kbhit() {
#ifdef _WIN32
    return _kbhit() != 0;
#else
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(1, &fds, nullptr, nullptr, &tv) > 0;
#endif
}

static int getch() {
#ifdef _WIN32
    return _getch();
#else
    char c;
    read(STDIN_FILENO, &c, 1);
    return static_cast<unsigned char>(c);
#endif
}

// ============================================================================
// Construction
// ============================================================================

TerminalUI::TerminalUI() {
    enable_ansi();
    // Clear screen
    std::cout << "\033[2J\033[H" << std::flush;
}

TerminalUI::~TerminalUI() {
    // Day 17: stop and join the agent thread before any members are torn down,
    // so the agent callback never holds a dangling `this` pointer.
    cancel_ = true;
    if (agent_thread_.joinable()) agent_thread_.join();

    // Reset the terminal so the shell isn't left broken (hidden cursor, stale
    // colors, or a cursor parked at a hardcoded row that may be out of bounds).
    // A fixed row like `\033[30;1H` can land outside a short buffer and confuse
    // PowerShell's PSReadLine (it tracks cursor state and crashes on paste).
    std::cout << "\033[0m"     // reset attributes
              << "\033[?25h"   // show cursor
              << "\033[0J"     // clear from cursor to end of screen
              << std::endl;    // fresh line so the shell prompt renders cleanly
}

// ============================================================================
// Thread-safe accessors
// ============================================================================

void TerminalUI::log(LogEntry::Kind kind, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back({kind, text});
    while (log_.size() > kMaxLogSize) log_.pop_front();
}

void TerminalUI::append_to_last(LogEntry::Kind kind, const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!log_.empty() && log_.back().kind == kind) {
        log_.back().text += text;
    } else {
        log_.push_back({kind, text});
        while (log_.size() > kMaxLogSize) log_.pop_front();
    }
}

void TerminalUI::set_status(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = text;
}

void TerminalUI::set_stats(int tokens, int iterations, double elapsed_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = tokens;
    iterations_ = iterations;
    elapsed_ms_ = elapsed_ms;
}

// ============================================================================
// ANSI helpers
// ============================================================================

const char* TerminalUI::ansi_color(LogEntry::Kind k) {
    switch (k) {
        case LogEntry::Kind::THINK:   return "\033[36m";  // cyan
        case LogEntry::Kind::ACT:     return "\033[33m";  // yellow
        case LogEntry::Kind::OBSERVE: return "\033[32m";  // green
        case LogEntry::Kind::RESULT:  return "\033[37;1m";// bold white
        case LogEntry::Kind::ERROR:   return "\033[31m";  // red
        case LogEntry::Kind::USER:    return "\033[35m";  // magenta
        default: return "\033[37m";   // white
    }
}

void TerminalUI::draw_box(int y, int x, int h, int w) const {
    std::cout << "\033[" << y << ";" << x << "H";
    // Top border
    std::cout << "\033[34m┌" << repeat_str("─", w - 2) << "┐";
    // Side borders
    for (int i = 1; i < h - 1; ++i) {
        std::cout << "\033[" << (y + i) << ";" << x << "H│"
                  << std::string(w - 2, ' ')
                  << "\033[" << (y + i) << ";" << (x + w - 1) << "H│";
    }
    // Bottom border
    std::cout << "\033[" << (y + h - 1) << ";" << x << "H└"
              << repeat_str("─", w - 2) << "┘\033[0m";
}

// ============================================================================
// Main render loop
// ============================================================================

void TerminalUI::render_frame(const std::string& input_line) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));

    // Hide cursor during rendering
    std::cout << "\033[?25l";

    // Dimensions (Day 19: query the terminal every frame so resizes apply).
    int W = 80, H = 24;
    get_terminal_size(W, H);
    const int CONV_Y = kStatusH + 1;
    const int CONV_H = conversation_height();
    const int INPUT_Y = CONV_Y + CONV_H + 1;

    // ── Status bar ──────────────────────────────────────────────────────────
    std::cout << "\033[1;1H\033[34m┌── Status" << repeat_str("─", W - 14) << "┐";
    std::cout << "\033[2;1H│\033[0m " << status_;

    // Stats on right side
    std::ostringstream stats_ss;
    stats_ss << "Tokens:" << tokens_ << " Iter:" << iterations_ << " "
             << std::fixed << std::setprecision(1);
    if (elapsed_ms_ >= 1000.0) stats_ss << (elapsed_ms_ / 1000.0) << "s";
    else stats_ss << elapsed_ms_ << "ms";
    stats_ss << (agent_busy_ ? " ⏳" : " ✓");
    std::string stats = stats_ss.str();

    int stats_pos = W - static_cast<int>(stats.size()) - 3;
    if (stats_pos > 30) {
        std::cout << "\033[2;" << stats_pos << "H\033[0m" << stats;
    }
    std::cout << "\033[3;1H\033[34m└" << repeat_str("─", W - 2) << "┘\033[0m";

    // ── Conversation ────────────────────────────────────────────────────────
    int conv_start = CONV_Y;
    // Clear conversation area
    for (int r = 0; r < CONV_H; ++r) {
        std::cout << "\033[" << (conv_start + r) << ";1H\033[K";
    }

    size_t total = log_.size();
    size_t visible = std::min(total, static_cast<size_t>(CONV_H - 1));

    // Day 19: scroll window. scroll_offset_ = how many entries are scrolled
    // back from the newest; clamp it into [0, total - visible].
    size_t max_offset = total > visible ? total - visible : 0;
    size_t offset = std::min(
        static_cast<size_t>(std::max(0, scroll_offset_)), max_offset);
    size_t start = total - offset - visible;

    for (size_t i = 0; i < visible; ++i) {
        const auto& entry = log_[start + i];
        const char* c = ansi_color(entry.kind);
        const char* emoji;
        switch (entry.kind) {
            case LogEntry::Kind::THINK:   emoji = "💭"; break;
            case LogEntry::Kind::ACT:     emoji = "🔧"; break;
            case LogEntry::Kind::OBSERVE: emoji = "👁"; break;
            case LogEntry::Kind::RESULT:  emoji = "🤖"; break;
            case LogEntry::Kind::ERROR:   emoji = "❌"; break;
            case LogEntry::Kind::USER:    emoji = "👤"; break;
            default: emoji = "  ";
        }

        // Truncate text to fit
        std::string line = entry.text;
        int max_len = W - 8;
        if (static_cast<int>(line.size()) > max_len)
            line = line.substr(0, max_len - 3) + "...";

        std::cout << "\033[" << (conv_start + static_cast<int>(i)) << ";2H"
                  << c << emoji << " " << line << "\033[0m";
    }

    // ── Input area ──────────────────────────────────────────────────────────
    std::cout << "\033[" << INPUT_Y << ";1H\033[34m┌── Input" << repeat_str("─", W - 13) << "┐";
    std::cout << "\033[" << (INPUT_Y + 1) << ";1H│\033[0m > " << input_line;
    // Cursor after input
    std::cout << "\033[" << (INPUT_Y + 1) << ";" << (input_line.size() + 5) << "H\033[?25h";

    std::cout << "\033[" << (INPUT_Y + 2) << ";1H\033[34m└" << repeat_str("─", W - 2) << "┘\033[0m";

    std::cout << std::flush;
}

// ============================================================================
// run()
// ============================================================================

void TerminalUI::run(std::function<std::string(const std::string&)> on_submit) {
    log(LogEntry::Kind::SYSTEM, "Agent ready. Type your message and press Enter.");

    std::string input_buffer;
    constexpr int FPS = 30;
    constexpr int FRAME_MS = 1000 / FPS;

    while (true) {
        render_frame(input_buffer);

        // Wait for input or frame timeout
        int waited = 0;
        while (waited < FRAME_MS) {
            if (kbhit()) {
                int c = getch();

                if (c == '\r' || c == '\n') {
                    // Submit
                    if (!input_buffer.empty() && !agent_busy_) {
                        std::string query = input_buffer;
                        input_buffer.clear();
                        scroll_offset_ = 0;   // Day 19: pin back to newest on submit
                        log(LogEntry::Kind::USER, query);

                        // Day 17: join any previous agent thread before
                        // starting a new one (defensive; agent_busy_ guards it).
                        if (agent_thread_.joinable()) agent_thread_.join();

                        agent_busy_ = true;

                        // Run agent on a tracked background thread
                        agent_thread_ = std::jthread([this, query, on_submit]() {
                            set_status("Thinking...");
                            auto t0 = std::chrono::steady_clock::now();

                            try {
                                std::string result = on_submit(query);
                                auto elapsed = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0).count();

                                log(LogEntry::Kind::RESULT, result);
                                set_status("Ready");
                                set_stats(0, 0, elapsed);
                            } catch (const std::exception& e) {
                                log(LogEntry::Kind::ERROR,
                                    std::string("Error: ") + e.what());
                                set_status("Error");
                            }
                            agent_busy_ = false;
                        });
                    }
                    break;
                } else if (c == 27) {
                    // Escape → cancel and quit
                    cancel_ = true;
                    if (agent_thread_.joinable()) agent_thread_.join();
                    return;
                } else if (c == 0 || c == 0xE0) {
                    // Extended key (Windows): arrows / PgUp / PgDn arrive as a
                    // two-byte sequence (prefix 0 or 0xE0 + scan code).
                    int k = getch();
                    const int page = conversation_height();
                    if (k == 72) {                       // ↑ Up: one line back
                        ++scroll_offset_;
                    } else if (k == 80) {                // ↓ Down: one line forward
                        scroll_offset_ = std::max(0, scroll_offset_ - 1);
                    } else if (k == 73) {                // PgUp: one page back
                        scroll_offset_ += page;
                    } else if (k == 81) {                // PgDn: one page forward
                        scroll_offset_ = std::max(0, scroll_offset_ - page);
                    }
                } else if (c == '\b' || c == 127) {
                    // Backspace
                    if (!input_buffer.empty()) input_buffer.pop_back();
                } else if (c >= 32 && c < 127) {
                    // Printable
                    input_buffer += static_cast<char>(c);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                waited += 1;
            }
        }
    }
}

}  // namespace cpp_agent
