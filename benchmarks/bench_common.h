/**
 * @file bench_common.h
 * @brief Shared helpers for all benchmarks: timing + percentile statistics.
 *
 * Every benchmark executable measures latency/throughput the same way:
 *   - run a callable `reps` times
 *   - collect per-run wall-clock durations (milliseconds)
 *   - report mean / P50 (median) / P99
 *
 * P50/P99 use nearest-rank percentiles over the sorted sample vector.
 * This keeps the code dependency-free (no external stats library) and
 * gives stable, honest numbers that map 1:1 onto benchmarks/README.md.
 */

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

/// Milliseconds elapsed since `t0`.
inline double elapsed_ms(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// Nearest-rank percentile over an ALREADY-SORTED vector.
/// p in [0, 100].
inline double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    // idx = ceil(p/100 * n) - 1, clamped to [0, n-1].
    auto idx = static_cast<size_t>(std::ceil(p / 100.0 * sorted.size()));
    idx = idx == 0 ? 0 : idx - 1;
    idx = std::min(idx, sorted.size() - 1);
    return sorted[idx];
}

/// Summary statistics over a set of per-run durations (ms).
struct Stats {
    double mean = 0.0;
    double p50  = 0.0;
    double p99  = 0.0;
    double min  = 0.0;
    double max  = 0.0;
    size_t samples = 0;
};

inline Stats summarize(std::vector<double> samples) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    Stats s;
    s.samples = samples.size();
    s.min = samples.front();
    s.max = samples.back();
    double sum = 0.0;
    for (double v : samples) sum += v;
    s.mean = sum / static_cast<double>(samples.size());
    s.p50 = percentile(samples, 50.0);
    s.p99 = percentile(samples, 99.0);
    return s;
}

/// Run `fn` `reps` times, returning per-run wall-clock statistics (ms).
/// `fn` is invoked with no arguments; its entire execution is timed.
template <typename Fn>
Stats run_reps(Fn&& fn, size_t reps) {
    std::vector<double> samples;
    samples.reserve(reps);
    for (size_t i = 0; i < reps; ++i) {
        auto t0 = Clock::now();
        fn();
        samples.push_back(elapsed_ms(t0));
    }
    return summarize(std::move(samples));
}

/// Print a table row aligned with the header printed by print_header().
inline void print_row(const std::string& name, const Stats& s,
                      const std::string& unit = "ms") {
    std::cout << std::left  << std::setw(38) << name
              << std::right << std::setw(10)
              << std::fixed << std::setprecision(3) << s.mean << " "
              << std::setw(10) << s.p50 << " "
              << std::setw(10) << s.p99 << "  " << unit << "\n";
}

inline void print_header() {
    std::cout << std::left  << std::setw(38) << "Benchmark"
              << std::right << std::setw(10) << "Mean"
              << std::setw(11) << "P50"
              << std::setw(11) << "P99"
              << "  Unit\n";
    std::cout << std::string(38, '-') << " "
              << std::string(10, '-') << " "
              << std::string(10, '-') << " "
              << std::string(10, '-') << "  ----\n";
}

}  // namespace bench
