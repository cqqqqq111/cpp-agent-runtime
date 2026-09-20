/**
 * @file bench_tools.cpp
 * @brief Day 20 Task 2 — tool throughput / latency benchmark.
 *
 * Measures each tool's pure execution time WITHOUT going through an LLM:
 *
 *   - FileSystemTool::read_file   across file sizes (1KB → 10MB)
 *   - GrepTool                     across file counts (10 → 500 files)
 *   - CodeExecTool                 subprocess startup latency (shell / python)
 *   - VectorIndex::search          recall latency across index sizes (1K/10K/100K)
 *
 * All timings are per-operation wall-clock (ms). File fixtures are created
 * under std::filesystem::temp_directory_path() and cleaned up on exit.
 */

#include "bench_common.h"
#include "memory/vector_index.h"
#include "tool/code_exec_tool.h"
#include "tool/fs_tool.h"
#include "tool/grep_tool.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <iostream>
#include <string>

using namespace cpp_agent;
namespace fs = std::filesystem;

// ============================================================================
// Fixture: a temp directory that cleans itself up
// ============================================================================
class TempDir {
public:
    TempDir() {
        root_ = fs::temp_directory_path() / "cpp-agent-bench-tools";
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    ~TempDir() { fs::remove_all(root_); }
    const fs::path& root() const { return root_; }

    /// Write a file of exactly `bytes` total size using a repeated line.
    /// Binary mode: text mode would translate '\n' → "\r\n" on Windows and
    /// inflate the file past the tool's 10 MB cap.
    void write_sized(const std::string& name, size_t bytes) {
        std::ofstream f(root_ / name, std::ios::binary);
        const std::string line =
            "benchmark line with some words to read back 0123456789 abcdefghij\n";
        size_t written = 0;
        while (written < bytes) {
            size_t remaining = bytes - written;
            if (remaining >= line.size()) {
                f << line;
                written += line.size();
            } else {
                f.write(line.data(), static_cast<std::streamsize>(remaining));
                written = bytes;
            }
        }
    }

private:
    fs::path root_;
};

// ============================================================================
// 1. FileSystemTool::read_file at different sizes
// ============================================================================
static void bench_read_file() {
    std::cout << "--- FileSystemTool::read_file (per call) ---\n";
    bench::print_header();

    TempDir dir;
    struct Case { const char* name; const char* file; size_t bytes; size_t reps; };
    const Case cases[] = {
        {"read_file 1KB",   "f_1k.bin",   1 * 1024,              20},
        {"read_file 100KB", "f_100k.bin", 100 * 1024,            10},
        {"read_file 1MB",   "f_1m.bin",   1 * 1024 * 1024,       5},
        {"read_file 10MB",  "f_10m.bin",  10 * 1024 * 1024,      3},
    };
    for (const auto& c : cases) {
        dir.write_sized(c.file, c.bytes);
    }

    FileSystemTool tool(dir.root());
    for (const auto& c : cases) {
        json args = {{"action", "read_file"}, {"path", c.file}};
        auto st = bench::run_reps([&] { tool.execute(args, "bench"); }, c.reps);
        bench::print_row(c.name, st);
    }
}

// ============================================================================
// 2. GrepTool across file counts
// ============================================================================
static void bench_grep() {
    std::cout << "\n--- GrepTool (per search, pattern matches all files) ---\n";
    bench::print_header();

    // One fixture per file-count so each timing sees an isolated directory.
    struct Case { int files; size_t reps; };
    const Case cases[] = {{10, 10}, {100, 10}, {500, 5}};

    for (const auto& c : cases) {
        TempDir dir;
        for (int i = 0; i < c.files; ++i) {
            std::ofstream f(dir.root() / ("file_" + std::to_string(i) + ".txt"));
            for (int line = 0; line < 20; ++line) {
                f << (line == 7 ? "needle_token_here\n" : "ordinary filler line\n");
            }
        }
        GrepTool tool(dir.root());
        json args = {{"pattern", "needle_token"}};
        auto st = bench::run_reps([&] { tool.execute(args, "bench"); }, c.reps);
        bench::print_row("grep " + std::to_string(c.files) + " files", st);
    }
}

// ============================================================================
// 3. CodeExecTool subprocess startup latency
// ============================================================================
static void bench_code_exec() {
    std::cout << "\n--- CodeExecTool (subprocess startup + run) ---\n";
    bench::print_header();

    CodeExecTool tool(std::chrono::seconds(5));

    {
        json args = {{"language", "shell"}, {"code", "echo hi"}};
        auto st = bench::run_reps([&] { tool.execute(args, "bench"); }, 10);
        bench::print_row("shell: echo hi", st);
    }
    {
        json args = {{"language", "python"}, {"code", "pass"}};
        auto st = bench::run_reps([&] { tool.execute(args, "bench"); }, 10);
        bench::print_row("python: pass", st);
    }
}

// ============================================================================
// 4. VectorIndex::search recall latency
// ============================================================================

/// Deterministic pseudo-random vector (LCG), dim 128 to keep memory bounded.
static std::vector<float> make_vec(size_t dim, uint32_t seed) {
    std::vector<float> v(dim);
    uint32_t x = seed;
    for (size_t i = 0; i < dim; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = (static_cast<float>(x & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    }
    return v;
}

static void bench_vector_index() {
    std::cout << "\n--- VectorIndex::search (top-10, dim=128) ---\n";
    bench::print_header();

    const size_t dim = 128;
    struct Case { const char* name; size_t n; size_t reps; };
    const Case cases[] = {{"search 1K vectors", 1000, 20},
                          {"search 10K vectors", 10000, 10},
                          {"search 100K vectors", 100000, 5}};

    for (const auto& c : cases) {
        VectorIndex idx;
        for (size_t i = 0; i < c.n; ++i) {
            idx.add(make_vec(dim, static_cast<uint32_t>(i)), "doc_" + std::to_string(i));
        }
        auto query = make_vec(dim, 0xDEADBEEFu);
        auto st = bench::run_reps([&] { idx.search(query, 10); }, c.reps);
        bench::print_row(c.name, st);
    }
}

int main() {
    spdlog::set_level(spdlog::level::off);  // keep benchmark output clean
    std::cout << "=== Tool throughput / latency (ms per op, lower is better) ===\n\n";
    bench_read_file();
    bench_grep();
    bench_code_exec();
    bench_vector_index();
    std::cout << "\nDone.\n";
    return 0;
}
