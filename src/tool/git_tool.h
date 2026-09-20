/**
 * @file git_tool.h
 * @brief GitTool — git operations for agent-assisted version control.
 *
 * Supports: status, diff, log.
 * All commands run via popen on the repo root.
 */

#pragma once

#include "tool/tool_base.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace cpp_agent {

class GitTool : public Tool {
public:
    explicit GitTool(fs::path repo_root = fs::current_path());

    std::string name() const override { return "git"; }

    std::string description() const override {
        return "Run git operations. 'action' can be 'status' (working tree), "
               "'diff' (unstaged changes), or 'log' (recent commits, "
               "optional 'count' parameter).";
    }

    json parameters_schema() const override;

    ToolResult execute(const json& args, const std::string& call_id) override;

private:
    fs::path repo_root_;

    /// Run a git command and capture output.
    std::string run_git(const std::string& subcommand);
};

}  // namespace cpp_agent
