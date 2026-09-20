/**
 * @file bench_scheduler.cpp
 * @brief Day 20 Task 1 — coroutine scheduler throughput benchmark.
 *
 * Measures the scheduling overhead of the single-threaded cooperative
 * scheduler (src/core/scheduler.h) across a sweep of workload shapes:
 *
 *   1. Many tasks, one iteration each   → 1K / 10K / 100K tasks
 *   2. Few tasks, many iterations each  → 100 tasks × 100 / 1K / 10K yields
 *   3. Parallel tool-execution simulation → N coroutines scheduled together,
 *      each yielding K times (cooperative "parallel" tool calls)
 *
 * Each cell is the wall-clock of ONE batch (schedule + run_all), averaged
 * over a few repetitions. Units are milliseconds per batch.
 */

#include "bench_common.h"
#include "core/scheduler.h"
#include "core/task.h"

#include <iostream>
#include <vector>

using namespace cpp_agent;

// ============================================================================
// Parallel tool-execution simulation
// ============================================================================
//
// Each coroutine stands in for one tool call that "waits on I/O" by yielding
// `yields` times. We schedule N of them together and drive them all to
// completion with run_all() — exactly how ReActLoop::execute_tools() runs
// parallel tool calls via its scheduler.
static Task<void> sim_tool(Scheduler& s, int yields) {
    for (int i = 0; i < yields; ++i) {
        co_await s.yield();
    }
    co_return;
}

static double parallel_batch(Scheduler& s, int num_tasks, int yields) {
    std::vector<Task<void>> tasks;
    tasks.reserve(static_cast<size_t>(num_tasks));
    for (int i = 0; i < num_tasks; ++i) {
        tasks.push_back(sim_tool(s, yields));
    }
    auto t0 = bench::Clock::now();
    for (auto& t : tasks) {
        s.schedule(t.handle());
    }
    s.run_all();
    return bench::elapsed_ms(t0);
}

int main() {
    std::cout << "=== Scheduler throughput (ms per batch, lower is better) ===\n\n";

    // ---- 1. Many tasks × 1 iteration --------------------------------------
    std::cout << "--- many tasks, 1 iteration each ---\n";
    bench::print_header();
    {
        Scheduler s;
        for (auto [label, n] : {std::pair{"1K  tasks x1", 1000},
                                std::pair{"10K tasks x1", 10000},
                                std::pair{"100K tasks x1", 100000}}) {
            auto st = bench::run_reps([&] { s.benchmark(n, 1); },
                                      n >= 100000 ? 3 : 7);
            bench::print_row(label, st);
        }
    }

    // ---- 2. Few tasks × many iterations -----------------------------------
    std::cout << "\n--- 100 tasks, many iterations each ---\n";
    bench::print_header();
    {
        Scheduler s;
        for (auto [label, k] : {std::pair{"100 tasks x100", 100},
                                std::pair{"100 tasks x1K", 1000},
                                std::pair{"100 tasks x10K", 10000}}) {
            auto st = bench::run_reps([&] { s.benchmark(100, k); },
                                      k >= 10000 ? 3 : 7);
            bench::print_row(label, st);
        }
    }

    // ---- 3. Parallel tool-execution simulation ----------------------------
    std::cout << "\n--- parallel tool-execution simulation (N tasks x5 yields) ---\n";
    bench::print_header();
    {
        Scheduler s;
        for (int n : {1, 2, 4, 8, 16, 32}) {
            auto st = bench::run_reps([&] { parallel_batch(s, n, 5); }, 7);
            bench::print_row("N=" + std::to_string(n) + " concurrent tools", st);
        }
    }

    std::cout << "\nDone.\n";
    return 0;
}
