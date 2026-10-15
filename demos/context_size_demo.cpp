// Local payload comparison; no Python initialization or API requests.
#include "tool/fs_tool.h"
#include "tool/grep_tool.h"
#include "tool/echo_tool.h"
#include <fmt/core.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <memory>

using namespace cpp_agent;
int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);
    auto root = std::filesystem::absolute(argc > 1 ? argv[1] : ".");
    ToolRegistry all;
    all.register_tool(std::make_unique<EchoTool>());
    all.register_tool(std::make_unique<FileSystemTool>(root));
    all.register_tool(std::make_unique<GrepTool>(root));
    ToolRegistry selected;
    selected.register_tool(std::make_unique<FileSystemTool>(root));
    auto full = selected.execute("filesystem", {{"action", "read_file"},
        {"path", "src/agent/react_loop.cpp"}}, "full");
    auto slice = selected.execute("filesystem", {{"action", "read_file"},
        {"path", "src/agent/react_loop.cpp"}, {"start_line", 150}, {"end_line", 180}}, "slice");
    if (!full.success || !slice.success) return 1;
    json result = {
        {"all_tool_schema_bytes", all.to_openai_tools().dump().size()},
        {"filesystem_schema_bytes", selected.to_openai_tools().dump().size()},
        {"full_file_result_bytes", full.content.size()},
        {"selected_lines_result_bytes", slice.content.size()},
        {"selected_start_line", 150}, {"selected_end_line", 180},
        {"note", "Payload bytes, not token counts or a measured API latency speedup"}
    };
    fmt::print("{}\n", result.dump(2));
}
