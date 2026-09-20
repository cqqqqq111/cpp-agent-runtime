/**
 * Day 2 tests: coroutine scheduler correctness + performance baseline.
 *
 * Build:  cmake --build build --target tests
 * Run:    ./build/tests/cpp-agent-tests
 */

#include "core/scheduler.h"
#include "core/task.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace cpp_agent;

// --------------------------------------------------------------------------
// Helper: a simple coroutine that increments a counter on each yield
// --------------------------------------------------------------------------
static Task<int> counting_coro(Scheduler& s, int id, int yields) {
    for (int i = 0; i < yields; ++i) {
        co_await s.yield();
    }
    co_return id * 100 + yields;
}

// --------------------------------------------------------------------------
// Test 1: single coroutine runs to completion
// --------------------------------------------------------------------------
static bool test_single_coroutine() {
    Scheduler s;
    auto task = counting_coro(s, 1, 5);
    s.schedule(task.handle());
    s.run_all();
    assert(task.done());
    assert(task.result() == 105);  // 1 * 100 + 5
    return true;
}

// --------------------------------------------------------------------------
// Test 2: three coroutines, verify interleaving order
// --------------------------------------------------------------------------
static bool test_interleaving_order() {
    Scheduler s;
    std::vector<int> log;

    // Each coroutine pushes its id when it runs, then yields.
    auto recorder = [&](Scheduler& s, int id) -> Task<void> {
        for (int i = 0; i < 3; ++i) {
            log.push_back(id);
            co_await s.yield();
        }
        co_return;
    };

    auto t0 = recorder(s, 0);
    auto t1 = recorder(s, 1);
    auto t2 = recorder(s, 2);

    s.schedule(t0.handle());
    s.schedule(t1.handle());
    s.schedule(t2.handle());
    s.run_all();

    // Round-robin: 0, 1, 2, 0, 1, 2, 0, 1, 2
    std::vector<int> expected = {0, 1, 2, 0, 1, 2, 0, 1, 2};
    assert(log == expected);

    assert(t0.done() && t1.done() && t2.done());
    return true;
}

// --------------------------------------------------------------------------
// Test 3: async-like chain — coroutine A spawns B mid-flight
// --------------------------------------------------------------------------
static bool test_dynamic_scheduling() {
    Scheduler s;
    std::vector<std::string> timeline;

    auto child = [&](Scheduler& s, int id) -> Task<void> {
        timeline.push_back("child_" + std::to_string(id) + "_start");
        co_await s.yield();
        timeline.push_back("child_" + std::to_string(id) + "_end");
        co_return;
    };

    // Child task lives here (outside the parent coroutine frame) so it
    // is NOT destroyed when the parent co_returns. C++ destroys local
    // variables BEFORE final_suspend; moving ownership out prevents
    // the child coroutine from being prematurely destroyed.
    Task<void> child_task;

    auto parent = [&](Scheduler& s) -> Task<void> {
        timeline.push_back("parent_start");
        co_await s.yield();

        // Spawn child mid-flight
        auto c = child(s, 1);
        s.schedule(c.handle());
        timeline.push_back("parent_spawned_child");

        // Move ownership to outer scope so c's handle survives
        // the parent's co_return (which destroys local variables
        // BEFORE final_suspend).
        child_task = std::move(c);

        co_await s.yield();

        timeline.push_back("parent_end");
        co_return;
    };

    auto p = parent(s);
    s.schedule(p.handle());
    s.run_all();

    // Verify interleaving:
    // parent_start → parent yields → parent_spawned_child → child starts
    // → child yields → parent_end → child resumes → child_end
    std::cout << "Timeline (" << timeline.size() << " entries):\n";
    for (size_t i = 0; i < timeline.size(); ++i) {
        std::cout << "  [" << i << "] " << timeline[i] << "\n";
    }

    assert(timeline.size() == 5);
    assert(timeline[0] == "parent_start");
    assert(timeline[1] == "parent_spawned_child");
    assert(timeline[2] == "child_1_start");
    assert(timeline[3] == "parent_end");
    assert(timeline[4] == "child_1_end");

    assert(p.done());
    assert(child_task.done());
    return true;
}

// --------------------------------------------------------------------------
// Test 4: exception propagation through coroutine
// --------------------------------------------------------------------------
static Task<void> failing_coro(Scheduler& s) {
    co_await s.yield();
    throw std::runtime_error("coroutine failure");
    co_return;
}

static bool test_exception_propagation() {
    Scheduler s;
    auto task = failing_coro(s);
    s.schedule(task.handle());
    s.run_all();

    assert(task.done());
    bool caught = false;
    try {
        task.result();
    } catch (const std::runtime_error& e) {
        caught = true;
        assert(std::string(e.what()) == "coroutine failure");
    }
    assert(caught);
    return true;
}

// --------------------------------------------------------------------------
// Test 5: 1000 empty tasks benchmark (must be < 10ms)
// --------------------------------------------------------------------------
static bool test_benchmark_1000_tasks() {
    Scheduler s;
    auto result = s.benchmark(1000, 1);

    std::cout << "Benchmark: " << result.tasks_executed << " tasks, "
              << result.total_iterations << " iterations, "
              << result.elapsed_ms << " ms\n";

    assert(result.tasks_executed == 1000);
    assert(result.total_iterations == 1000);
    assert(result.elapsed_ms < 10.0);

    return true;
}

// --------------------------------------------------------------------------
// Test 6: heavy benchmark — 100 tasks × 100 yields each
// --------------------------------------------------------------------------
static bool test_benchmark_heavy() {
    Scheduler s;
    auto result = s.benchmark(100, 100);

    std::cout << "Heavy benchmark: " << result.tasks_executed << " tasks, "
              << result.total_iterations << " iterations ("
              << result.total_iterations / 1000 << "K), "
              << result.elapsed_ms << " ms\n";

    assert(result.total_iterations == 10000);
    return true;
}

// --------------------------------------------------------------------------
// Runner
// --------------------------------------------------------------------------
int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"single coroutine completion", test_single_coroutine},
        {"interleaving order (round-robin)", test_interleaving_order},
        {"dynamic spawning mid-flight", test_dynamic_scheduling},
        {"exception propagation", test_exception_propagation},
        {"benchmark: 1000 tasks < 10ms", test_benchmark_1000_tasks},
        {"benchmark: 10K iterations", test_benchmark_heavy},
    };

    int passed = 0, failed = 0;

    for (auto& [name, fn] : tests) {
        std::cout << "[TEST] " << name << " ... ";
        try {
            if (fn()) {
                std::cout << "PASSED\n";
                ++passed;
            } else {
                std::cout << "FAILED (assertion)\n";
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cout << "FAILED (exception: " << e.what() << ")\n";
            ++failed;
        }
    }

    std::cout << "\n=== " << passed << "/" << (passed + failed) << " passed ===\n";
    return failed > 0 ? 1 : 0;
}
