#include "tool/code_exec_tool.h"
#include "tool/process.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace cpp_agent {

// ============================================================================
// CodeExecTool
// ============================================================================

CodeExecTool::CodeExecTool(std::chrono::seconds timeout, size_t max_output_bytes)
    : timeout_(timeout), max_output_bytes_(max_output_bytes) {}

json CodeExecTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"language", {
                {"type", "string"},
                {"enum", {"python", "shell"}},
                {"description", "Language to execute: 'python' or 'shell'."}
            }},
            {"code", {
                {"type", "string"},
                {"description", "The source code or shell command to execute."}
            }}
        }},
        {"required", json::array({"language", "code"})}
    };
}

ToolResult CodeExecTool::execute(const json& args, const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    std::string language = args.value("language", "");
    std::string code = args.value("code", "");

    if (code.empty()) {
        tr.content = "Error: 'code' parameter is empty.";
        tr.success = false;
        return tr;
    }

    std::vector<std::string> command;
    if (language == "python") {
        command = {CPP_AGENT_PYTHON_EXECUTABLE, "-c", code};
    } else if (language == "shell") {
#ifdef _WIN32
        command = {"cmd.exe", "/D", "/S", "/C", code};
#else
        command = {"/bin/sh", "-c", code};
#endif
    } else {
        tr.content = "Error: unknown language '" + language + "'. Use 'python' or 'shell'.";
        tr.success = false;
        return tr;
    }

    spdlog::info("CodeExecTool: executing {} ({} chars)", language, code.size());

    try {
        auto result = run_process(command, timeout_, max_output_bytes_);
        tr.content = result.describe();
        tr.success = result.success();
    } catch (const std::exception& e) {
        tr.content = std::string("Execution error: ") + e.what();
        tr.success = false;
    }

    return tr;
}

}  // namespace cpp_agent
