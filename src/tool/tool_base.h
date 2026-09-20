/**
 * @file tool_base.h
 * @brief Tool abstraction — the plugin interface for agent capabilities.
 *
 * A Tool is anything the Agent can invoke: file I/O, code execution,
 * web search, git operations, etc. The Tool system is:
 *
 *   Tool (abstract base)  ←  one implementation per capability
 *        │
 *   ToolRegistry          ←  owns all tools, dispatches by name
 *        │
 *   ToolDefinition        ←  the JSON Schema metadata the LLM sees
 *
 * Flow:
 *   1. LLM returns a ToolCall (name + arguments JSON)
 *   2. Agent loop calls ToolRegistry::execute(name, arguments)
 *   3. Registry finds the Tool, calls Tool::execute(json)
 *   4. ToolResult is converted to a Message and fed back to the LLM
 *
 * Day 3 scope: EchoTool + FileSystemTool stubs.
 * Day 10 adds CodeExecTool, GrepTool, GitTool.
 */

#pragma once

#include "agent/message.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cpp_agent {

// ============================================================================
// ToolDefinition — metadata the LLM needs to call a tool
// ============================================================================
struct ToolDefinition {
    std::string name;        // unique identifier, e.g. "read_file"
    std::string description; // natural-language description for the LLM
    json parameters;         // JSON Schema for the arguments

    /// Convert to the OpenAI function-definition format.
    json to_openai_function() const {
        return {
            {"type", "function"},
            {"function", {
                {"name", name},
                {"description", description},
                {"parameters", parameters}
            }}
        };
    }
};

// ============================================================================
// Tool — abstract base class
// ============================================================================
class Tool {
public:
    virtual ~Tool() = default;

    /// Human-readable name, e.g. "read_file". Must be unique in a registry.
    virtual std::string name() const = 0;

    /// Description the LLM sees when deciding whether to call this tool.
    virtual std::string description() const = 0;

    /// JSON Schema for the arguments object. See https://json-schema.org/.
    virtual json parameters_schema() const = 0;

    /// Execute the tool with parsed JSON arguments.
    /// @param args  The parsed JSON arguments object.
    /// @param call_id  The tool_call id from the LLM (for the result message).
    /// @returns ToolResult with content and success flag.
    virtual ToolResult execute(const json& args, const std::string& call_id) = 0;

    /// Convenience: build a ToolDefinition for registration / LLM formatting.
    ToolDefinition definition() const {
        return ToolDefinition{name(), description(), parameters_schema()};
    }
};

// ============================================================================
// ToolRegistry — owns tools and dispatches by name
// ============================================================================
class ToolRegistry {
public:
    ToolRegistry() = default;

    /// Register a tool. Returns false if a tool with the same name already exists.
    bool register_tool(std::unique_ptr<Tool> tool);

    /// Remove a tool by name. Returns false if not found.
    bool unregister(const std::string& name);

    /// Find a tool by name. Returns nullptr if not found.
    Tool* find(const std::string& name) const;

    /// List all registered tool names.
    std::vector<std::string> names() const;

    /// Execute a tool call by name. Throws std::runtime_error if not found.
    ToolResult execute(const std::string& name, const json& args,
                       const std::string& call_id);

    /// Generate the OpenAI-compatible list of all registered tools.
    json to_openai_tools() const;

    /// Number of registered tools.
    size_t size() const noexcept { return tools_.size(); }

private:
    // Map from tool name to owned Tool. Using unordered_map for O(1) lookup.
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace cpp_agent
