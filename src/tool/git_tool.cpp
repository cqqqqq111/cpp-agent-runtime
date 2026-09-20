#include "tool/git_tool.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace cpp_agent {

// ============================================================================
// Helper: run a command and capture output
// ============================================================================
static std::string capture_command(const std::string& cmd) {
    struct PipeCloser { void operator()(FILE* f) const { if (f) _pclose(f); } };
    std::unique_ptr<FILE, PipeCloser> pipe(_popen(cmd.c_str(), "r"));
    if (!pipe) return "(error: popen failed)";

    std::ostringstream out;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe.get())) {
        out << buf.data();
    }
    std::string result = out.str();
    return result.empty() ? "(no output)" : result;
}

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

    if (action == "status") {
        tr.content = run_git("status --short");
        tr.success = true;
    } else if (action == "diff") {
        tr.content = run_git("diff");
        tr.success = true;
    } else if (action == "log") {
        int count = args.value("count", 10);
        tr.content = run_git("log --oneline -" + std::to_string(count));
        tr.success = true;
    } else {
        tr.content = "Error: unknown action '" + action +
                     "'. Use 'status', 'diff', or 'log'.";
        tr.success = false;
    }

    spdlog::debug("GitTool: {} → {} chars", action, tr.content.size());
    return tr;
}

std::string GitTool::run_git(const std::string& subcommand) {
    std::string cmd = "cd /d \"" + repo_root_.string() +
                      "\" && git " + subcommand + " 2>&1";
    try {
        return capture_command(cmd);
    } catch (const std::exception& e) {
        return std::string("git error: ") + e.what();
    }
}

}  // namespace cpp_agent
