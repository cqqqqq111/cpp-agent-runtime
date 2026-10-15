#include "tool/tool_base.h"

#include <spdlog/spdlog.h>
#include <stdexcept>

namespace cpp_agent {

// ============================================================================
// ToolRegistry
// ============================================================================

bool ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    if (!tool) return false;
    const auto& n = tool->name();
    if (tools_.contains(n)) {
        spdlog::warn("ToolRegistry: tool '{}' already registered, skipping.", n);
        return false;
    }
    spdlog::debug("ToolRegistry: registered tool '{}'", n);
    tools_[n] = std::move(tool);
    return true;
}

bool ToolRegistry::unregister(const std::string& name) {
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    tools_.erase(it);
    spdlog::debug("ToolRegistry: unregistered tool '{}'", name);
    return true;
}

Tool* ToolRegistry::find(const std::string& name) const {
    auto it = tools_.find(name);
    return (it != tools_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> ToolRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(tools_.size());
    for (const auto& [name, _] : tools_) {
        result.push_back(name);
    }
    return result;
}

ToolResult ToolRegistry::execute(const std::string& name, const json& args,
                                  const std::string& call_id) {
    auto* tool = find(name);
    if (!tool) {
        spdlog::error("ToolRegistry: tool '{}' not found.", name);
        ToolResult tr;
        tr.tool_call_id = call_id;
        tr.name = name;
        tr.content = "Error: tool '" + name + "' is not registered.";
        tr.success = false;
        return tr;
    }

    spdlog::debug("ToolRegistry: executing tool '{}' (call_id={})", name, call_id);
    try {
        return tool->execute(args, call_id);
    } catch (const std::exception& e) {
        spdlog::error("ToolRegistry: tool '{}' threw: {}", name, e.what());
        ToolResult tr;
        tr.tool_call_id = call_id;
        tr.name = name;
        tr.content = std::string("Tool execution error: ") + e.what();
        tr.success = false;
        return tr;
    }
}

json ToolRegistry::to_openai_tools() const {
    json tools = json::array();
    tools.get_ref<json::array_t&>().reserve(tools_.size());
    for (const auto& [_, tool] : tools_) {
        tools.push_back(tool->definition().to_openai_function());
    }
    return tools;
}

}  // namespace cpp_agent
