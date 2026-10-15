#pragma once
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace cpp_agent {
struct ProcessResult {
    std::string output;
    int exit_code = -1;
    bool timed_out = false;
    bool truncated = false;
    bool success() const { return !timed_out && exit_code == 0; }
    std::string describe() const;
};

// Executes argv without a shell. Captures stdout and stderr, bounds retained
// output, terminates the process group/job on timeout and reaps the child.
ProcessResult run_process(const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout,
                          size_t max_bytes = 10 * 1024);
} // namespace cpp_agent
