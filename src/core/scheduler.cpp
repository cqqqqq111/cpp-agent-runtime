#include "core/scheduler.h"
#include "core/task.h"

#include <chrono>
#include <vector>

namespace cpp_agent {

// ============================================================================
// Benchmark: schedule N empty tasks that each yield K times
// ============================================================================

/// Internal: a coroutine that yields N times then returns.
static Task<void> benchmark_coroutine(Scheduler& s, size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        co_await s.yield();
    }
    co_return;
}

Scheduler::BenchmarkResult Scheduler::benchmark(
    size_t num_tasks, size_t iterations_per_task
) {
    using clock = std::chrono::high_resolution_clock;

    // Create all tasks (they start suspended).
    std::vector<Task<void>> tasks;
    tasks.reserve(num_tasks);
    for (size_t i = 0; i < num_tasks; ++i) {
        tasks.push_back(benchmark_coroutine(*this, iterations_per_task));
    }

    // Schedule all tasks.
    auto t0 = clock::now();
    for (auto& t : tasks) {
        schedule(t.handle());
    }

    // Run until everything completes.
    run_all();

    auto elapsed = std::chrono::duration<double, std::milli>(clock::now() - t0);

    return BenchmarkResult{
        .tasks_executed = num_tasks,
        .total_iterations = num_tasks * iterations_per_task,
        .elapsed_ms = elapsed.count(),
    };
}

}  // namespace cpp_agent
