/**
 * @file code_exec_tool.h
 * @brief CodeExecTool — execute Python/shell commands in a subprocess.
 *
 * Security model:
 *   - Child process runs isolated from the agent process
 *   - Configurable timeout (default 5s) prevents runaway execution
 *   - Output capped at 10KB to avoid memory blowup
 *   - Only stdout is captured; stderr goes to agent logs
 */

#pragma once

#include "tool/tool_base.h"

#include <chrono>
#include <string>

namespace cpp_agent {

class CodeExecTool : public Tool {
public:
    explicit CodeExecTool(std::chrono::seconds timeout = std::chrono::seconds(5),
                          size_t max_output_bytes = 10 * 1024);

    std::string name() const override { return "execute_code"; }

    std::string description() const override {
        return "Execute a Python or shell command and return its output. "
               "Use 'language' ('python' or 'shell') and 'code' (the script). "
               "Timeout: " + std::to_string(timeout_.count()) + "s.";
    }

    json parameters_schema() const override;

    ToolResult execute(const json& args, const std::string& call_id) override;

private:
    std::chrono::seconds timeout_;
    size_t max_output_bytes_;

    /// Run a command via popen and capture output with timeout.
    std::string run_command(const std::string& cmd);
};

}  // namespace cpp_agent
