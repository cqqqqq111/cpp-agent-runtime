/**
 * @file echo_tool.h
 * @brief EchoTool — the simplest possible Tool, for testing the tool pipeline.
 *
 * Takes any input string and echoes it back with a prefix. Useful as a
 * canary to verify that the full tool chain (LLM → ToolCall → Registry →
 * Tool::execute → ToolResult → Message → LLM) is wired correctly.
 */

#pragma once

#include "tool/tool_base.h"

namespace cpp_agent {

class EchoTool : public Tool {
public:
    std::string name() const override { return "echo"; }

    std::string description() const override {
        return "Echoes back the input message. Use this to verify "
               "that the tool-calling pipeline is working correctly.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"message", {
                    {"type", "string"},
                    {"description", "The message to echo back."}
                }}
            }},
            {"required", json::array({"message"})}
        };
    }

    ToolResult execute(const json& args, const std::string& call_id) override {
        std::string msg = args.value("message", "(no message provided)");

        ToolResult result;
        result.tool_call_id = call_id;
        result.name = name();
        result.content = "[echo] " + msg;
        result.success = true;
        return result;
    }
};

}  // namespace cpp_agent
