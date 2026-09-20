/**
 * @file task.h
 * @brief Task<T> — C++20 coroutine wrapper with manual lifetime control.
 *
 * A Task<T> is the return type of a coroutine function. It owns the
 * coroutine frame and provides:
 *   - A coroutine_handle for the scheduler to drive
 *   - Result retrieval after completion
 *   - RAII cleanup via destructor (move-only)
 *
 * Lifecycle:
 *   1. coroutine function called  → Task<T> constructed, coroutine SUSPENDED
 *   2. scheduler.schedule(t.handle())  → handle queued
 *   3. scheduler.run_one() → handle.resume() → runs until next suspension
 *   4. ... (cycle repeats with co_await scheduler.yield())
 *   5. coroutine reaches co_return → final_suspend → handle.done() == true
 *   6. caller reads t.result() → Task destructor destroys handle
 */

#pragma once

#include <coroutine>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cpp_agent {

template <typename T = void>
class Task;

// ---------------------------------------------------------------------------
// promise_type — controls the coroutine's lifecycle
// ---------------------------------------------------------------------------

namespace detail {

struct promise_base {
    std::exception_ptr exception_ = nullptr;

    void unhandled_exception() noexcept { exception_ = std::current_exception(); }

    // Start suspended: the caller must explicitly schedule the coroutine.
    std::suspend_always initial_suspend() noexcept { return {}; }

    // Final suspend: also suspend_always. The handle stays alive so the
    // caller can read the result, then manually destroy.
    std::suspend_always final_suspend() noexcept { return {}; }
};

}  // namespace detail

template <typename T>
class Task {
public:
    struct promise_type : detail::promise_base {
        T result_{};

        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        void return_value(T value) noexcept { result_ = std::move(value); }
    };

    // ---- Move-only -----------------------------------------------------------
    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() {
        if (handle_) handle_.destroy();
    }

    // ---- Accessors -----------------------------------------------------------

    /// Has the coroutine completed (reached final_suspend)?
    bool done() const noexcept { return handle_ && handle_.done(); }

    /// True if this Task owns a coroutine (not default-constructed).
    bool valid() const noexcept { return handle_ != nullptr; }

    /// Raw handle for the scheduler. Valid until the Task is destroyed.
    std::coroutine_handle<> handle() const noexcept {
        return handle_;
    }

    /// Extract the result. ONLY call after done() == true.
    /// @throws std::exception_ptr if the coroutine threw an exception.
    T result() {
        if (!done()) throw std::runtime_error("Task::result() called before coroutine finished");
        auto& promise = handle_.promise();
        if (promise.exception_) {
            std::rethrow_exception(promise.exception_);
        }
        return std::move(promise.result_);
    }

private:
    std::coroutine_handle<promise_type> handle_ = nullptr;
};

// ---- void specialization --------------------------------------------------
template <>
class Task<void> {
public:
    struct promise_type : detail::promise_base {
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        void return_void() noexcept {}
    };

    Task() noexcept = default;
    explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { if (handle_) handle_.destroy(); }

    bool done() const noexcept { return handle_ && handle_.done(); }
    bool valid() const noexcept { return handle_ != nullptr; }
    std::coroutine_handle<> handle() const noexcept { return handle_; }

    void result() {
        if (!done()) throw std::runtime_error("Task::result() called before coroutine finished");
        auto& promise = handle_.promise();
        if (promise.exception_) {
            std::rethrow_exception(promise.exception_);
        }
    }

private:
    std::coroutine_handle<promise_type> handle_ = nullptr;
};

}  // namespace cpp_agent
