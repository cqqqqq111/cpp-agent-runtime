/**
 * Day 3 tests: Message serialization, MessageHistory truncation, Tool pipeline.
 */

#include "agent/message.h"
#include "tool/echo_tool.h"
#include "tool/tool_base.h"

#include <cassert>
#include <iostream>
#include <stdexcept>

using namespace cpp_agent;

// --------------------------------------------------------------------------
// Test 1: Message JSON round-trip
// --------------------------------------------------------------------------
static bool test_message_json_roundtrip() {
    // Simple user message
    auto m1 = Message::user("Hello, world!");
    json j1 = m1.to_json();
    auto m2 = Message::from_json(j1);
    assert(m2.role == "user");
    assert(m2.content == "Hello, world!");

    // Assistant with tool calls
    auto mc = Message::system("You are a helpful assistant.");
    json jc = mc.to_json();
    auto mc2 = Message::from_json(jc);
    assert(mc2.role == "system");
    assert(mc2.content == "You are a helpful assistant.");

    // Tool result
    auto mt = Message::tool_result("call_123", "echo", "hello back");
    json jt = mt.to_json();
    auto mt2 = Message::from_json(jt);
    assert(mt2.role == "tool");
    assert(mt2.tool_call_id == "call_123");
    assert(mt2.name == "echo");
    assert(mt2.content == "hello back");

    return true;
}

// --------------------------------------------------------------------------
// Test 2: ToolCall serialization
// --------------------------------------------------------------------------
static bool test_tool_call_json() {
    ToolCall tc;
    tc.id = "call_abc";
    tc.function_name = "echo";
    tc.arguments = R"({"message": "hi"})";

    json j = tc.to_json();
    auto parsed = ToolCall::from_json(j);

    assert(parsed.id == "call_abc");
    assert(parsed.function_name == "echo");
    assert(parsed.arguments == R"({"message": "hi"})");

    // Parsed arguments
    auto args = parsed.parsed_arguments();
    assert(args.has_value());
    assert((*args)["message"] == "hi");

    return true;
}

// --------------------------------------------------------------------------
// Test 3: MessageHistory basic operations
// --------------------------------------------------------------------------
static bool test_message_history_basic() {
    MessageHistory history;

    history.set_system_message(Message::system("You are an agent."));
    history.push(Message::user("Question 1"));
    history.push(Message::assistant("Answer 1"));
    history.push(Message::user("Question 2"));

    assert(history.size() == 3);  // system not counted
    assert(history.system_message().has_value());
    assert(history.estimated_tokens() > 0);

    return true;
}

// --------------------------------------------------------------------------
// Test 4: MessageHistory truncation
// --------------------------------------------------------------------------
static bool test_message_history_truncation() {
    MessageHistory history;
    history.set_system_message(Message::system("System prompt here. Quite long actually."));

    // Add many messages to build up tokens
    for (int i = 0; i < 20; ++i) {
        history.push(Message::user("This is a reasonably long user message number " +
                                   std::to_string(i) + " with extra padding text."));
    }

    size_t before = history.size();
    size_t tokens_before = history.estimated_tokens();

    // Truncate to a small budget — should remove most messages
    size_t removed = history.truncate_to(100);
    size_t after = history.size();

    std::cout << "  Before: " << before << " msgs, ~" << tokens_before << " tokens\n";
    std::cout << "  After:  " << after << " msgs, ~" << history.estimated_tokens() << " tokens\n";
    std::cout << "  Removed: " << removed << " messages\n";

    assert(removed > 0);
    assert(after < before);
    assert(history.estimated_tokens() <= 120);  // slightly over 100 is OK (single message granularity)

    // System message must survive
    assert(history.system_message().has_value());

    return true;
}

// --------------------------------------------------------------------------
// Test 5: EchoTool full pipeline (register → find → execute)
// --------------------------------------------------------------------------
static bool test_echo_tool_pipeline() {
    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    assert(registry.size() == 1);
    assert(registry.find("echo") != nullptr);
    assert(registry.find("nonexistent") == nullptr);
    assert(registry.names().size() == 1);
    assert(registry.names()[0] == "echo");

    // Execute
    json args = {{"message", "Hello Tool!"}};
    auto result = registry.execute("echo", args, "call_001");

    assert(result.success);
    assert(result.content == "[echo] Hello Tool!");
    assert(result.tool_call_id == "call_001");
    assert(result.name == "echo");

    // Execute nonexistent tool
    auto bad = registry.execute("no_such_tool", args, "call_002");
    assert(!bad.success);
    assert(bad.content.find("not registered") != std::string::npos);

    return true;
}

// --------------------------------------------------------------------------
// Test 6: ToolRegistry OpenAI format
// --------------------------------------------------------------------------
static bool test_openai_tool_format() {
    ToolRegistry registry;
    registry.register_tool(std::make_unique<EchoTool>());

    json tools = registry.to_openai_tools();
    assert(tools.is_array());
    assert(tools.size() == 1);

    auto& tool = tools[0];
    assert(tool["type"] == "function");
    assert(tool["function"]["name"] == "echo");
    assert(tool["function"]["description"].is_string());
    assert(tool["function"]["parameters"]["type"] == "object");

    return true;
}

// --------------------------------------------------------------------------
// Test 7: ToolDefinition serialization
// --------------------------------------------------------------------------
static bool test_tool_definition_roundtrip() {
    EchoTool echo;
    auto def = echo.definition();

    assert(def.name == "echo");
    assert(!def.description.empty());

    json openai = def.to_openai_function();
    assert(openai["type"] == "function");
    assert(openai["function"]["name"] == "echo");

    return true;
}

// --------------------------------------------------------------------------
// Test 8: Duplicate tool registration
// --------------------------------------------------------------------------
static bool test_duplicate_registration() {
    ToolRegistry registry;
    assert(registry.register_tool(std::make_unique<EchoTool>()));
    assert(registry.size() == 1);

    // Second registration with same name should fail
    assert(!registry.register_tool(std::make_unique<EchoTool>()));
    assert(registry.size() == 1);

    return true;
}

// --------------------------------------------------------------------------
// Runner
// --------------------------------------------------------------------------
int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"Message JSON round-trip", test_message_json_roundtrip},
        {"ToolCall JSON serialization", test_tool_call_json},
        {"MessageHistory basic ops", test_message_history_basic},
        {"MessageHistory truncation", test_message_history_truncation},
        {"EchoTool register → find → execute", test_echo_tool_pipeline},
        {"ToolRegistry OpenAI format", test_openai_tool_format},
        {"ToolDefinition serialization", test_tool_definition_roundtrip},
        {"Duplicate registration rejected", test_duplicate_registration},
    };

    int passed = 0, failed = 0;

    for (auto& [name, fn] : tests) {
        std::cout << "[TEST] " << name << " ... ";
        try {
            if (fn()) {
                std::cout << "PASSED\n";
                ++passed;
            } else {
                std::cout << "FAILED\n";
                ++failed;
            }
        } catch (const std::exception& e) {
            std::cout << "FAILED (" << e.what() << ")\n";
            ++failed;
        }
    }

    std::cout << "\n=== " << passed << "/" << (passed + failed) << " passed ===\n";
    return failed > 0 ? 1 : 0;
}
