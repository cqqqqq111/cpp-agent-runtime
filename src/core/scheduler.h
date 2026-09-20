/**
 * @file scheduler.h
 * @brief Single-threaded cooperative coroutine scheduler.
 *
 * The Scheduler maintains a FIFO queue of coroutine handles. Coroutines
 * voluntarily yield via `co_await scheduler.yield()`, which suspends them
 * and re-queues their handle at the back of the line.
 *
 * This is the simplest possible scheduler — no preemption, no priorities,
 * no thread pool. It maps 1:1 to the agent execution model: one "turn"
 * is one coroutine running until it hits I/O (LLM call or tool exec),
 * then yielding back to the scheduler so another agent can make progress.
 *
 * Usage:
 *   Scheduler s;
 *   auto task = my_coroutine(s);
 *   s.schedule(task.handle());
 *   s.run_all();  // or s.run_one() in a loop
 *   int result = task.result();
 */

#pragma once

#include <coroutine>
#include <queue>

namespace cpp_agent {

class Scheduler {
public:
    Scheduler() = default;
    ~Scheduler() = default;

    // Non-copyable, non-movable (awaiters hold raw pointers to queue_).
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // ---- API ---------------------------------------------------------------

    /// Queue a coroutine for execution. Safe to call from anywhere.
    void schedule(std::coroutine_handle<> h) { queue_.push(h); }

    /// Dequeue one handle and resume it. Returns true if work was done.
    /// - If the coroutine yielded, it re-queued itself (in yield()'s
    ///   await_suspend), so the queue may still have work.
    /// - If the coroutine finished, its handle is now done(); the caller
    ///   is responsible for destroying the handle (Task::~Task does this).
    bool run_one() {
        if (queue_.empty()) return false;
        auto h = queue_.front();
        queue_.pop();
        h.resume();
        return true;
    }

    /// Run until the queue is empty. All coroutines have either completed
    /// or are awaiting external I/O (not implemented Day 2).
    void run_all() {
        while (run_one()) {}
    }

    /// Number of pending coroutines in the queue.
    size_t pending() const noexcept { return queue_.size(); }

    /// True if nothing is queued.
    bool empty() const noexcept { return queue_.empty(); }

    // ---- Yield -------------------------------------------------------------
    //
    // Inside a coroutine, write:  co_await scheduler.yield();
    // This suspends the current coroutine and places it at the back of the
    // scheduler's queue, allowing other coroutines to make progress.

    class yield_awaiter {
    public:
        explicit yield_awaiter(std::queue<std::coroutine_handle<>>& q) noexcept
            : queue_(q) {}

        // Always suspend — we voluntarily yield.
        bool await_ready() const noexcept { return false; }

        // Push our handle back to the queue, then suspend.
        void await_suspend(std::coroutine_handle<> h) noexcept {
            queue_.push(h);
        }

        void await_resume() const noexcept {}

    private:
        std::queue<std::coroutine_handle<>>& queue_;
    };

    /// Returns an awaitable; co_await it inside a coroutine to yield.
    yield_awaiter yield() noexcept { return yield_awaiter{queue_}; }

    // ---- Benchmarking helpers ----------------------------------------------

    struct BenchmarkResult {
        size_t tasks_executed = 0;
        size_t total_iterations = 0;
        double elapsed_ms = 0.0;
    };

    /// Run N empty tasks and measure the scheduling overhead.
    /// Each task yields `iterations` times before completing.
    BenchmarkResult benchmark(size_t num_tasks, size_t iterations_per_task);

private:
    std::queue<std::coroutine_handle<>> queue_;
};

}  // namespace cpp_agent
