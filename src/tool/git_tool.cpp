#include "tool/git_tool.h"
#include "tool/process.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace cpp_agent {

// ============================================================================
// GitTool
// ============================================================================

GitTool::GitTool(fs::path repo_root) : repo_root_(std::move(repo_root)) {}

json GitTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"enum", {"status", "diff", "log"}},
                {"description", "Git operation: status, diff, or log."}
            }},
            {"count", {
                {"type", "integer"},
                {"description", "Number of commits for 'log' (default 10)."}
            }}
        }},
        {"required", json::array({"action"})}
    };
}

ToolResult GitTool::execute(const json& args, const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    std::string action = args.value("action", "");

    std::vector<std::string> command{"git", "-C", repo_root_.string()};
    if (action == "status") {
        command.insert(command.end(), {"status", "--short"});
    } else if (action == "diff") {
        command.push_back("diff");
    } else if (action == "log") {
        int count = args.value("count", 10);
        if (count < 1 || count > 1000) {
            tr.content = "Error: count must be between 1 and 1000.";
            tr.success = false;
            return tr;
        }
        command.insert(command.end(), {"log", "--oneline", "-n", std::to_string(count)});
    } else {
        tr.content = "Error: unknown action '" + action + "'. Use 'status', 'diff', or 'log'.";
        tr.success = false;
        return tr;
    }
    try {
        auto result = run_process(command, std::chrono::seconds(10), 1024 * 1024);
        tr.content = result.describe();
        tr.success = result.success();
    } catch (const std::exception& e) {
        tr.content = std::string("git error: ") + e.what();
        tr.success = false;
    }

    spdlog::debug("GitTool: {} → {} chars", action, tr.content.size());
    return tr;
}

}  // namespace cpp_agent
