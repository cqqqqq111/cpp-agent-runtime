/**
 * @file grep_tool.h
 * @brief GrepTool — regex search across files (code-aware grep).
 */

#pragma once

#include "tool/tool_base.h"

#include <filesystem>
#include <regex>
#include <string>

namespace fs = std::filesystem;

namespace cpp_agent {

class GrepTool : public Tool {
public:
    explicit GrepTool(fs::path root = fs::current_path());

    std::string name() const override { return "grep"; }

    std::string description() const override {
        return "Search for a regex pattern in files. "
               "Provide 'pattern' (regex), optional 'path' (directory or file), "
               "and optional 'glob' to filter files (e.g. '*.cpp'). "
               "Results include file:line:content.";
    }

    json parameters_schema() const override;

    ToolResult execute(const json& args, const std::string& call_id) override;

private:
    fs::path root_;
};

}  // namespace cpp_agent
