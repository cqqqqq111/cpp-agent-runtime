#include "demos/demo_common.h"
#include <filesystem>
#include <thread>

using namespace cpp_agent;
using namespace std::chrono_literals;
class WaitTool : public Tool {
public:
    std::string name() const override { return "wait"; }
    std::string description() const override { return "Synthetic 10ms blocking tool"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&, const std::string& id) override {
        std::this_thread::sleep_for(10ms);
        return {id, name(), "done", true};
    }
};
class TimedLLM : public LLMAdapter {
public:
    LLMResponse chat(const std::string&, const std::string&) override { return {}; }
    LLMResponse chat_full(const std::vector<Message>& messages, const json&) override {
        std::this_thread::sleep_for(5ms);
        LLMResponse response;
        if (messages.back().is_tool_result()) response.content = "Performance scenario complete.";
        else {
            response.has_tool_calls = true;
            response.tool_calls = {{"a", "wait", "{}"}, {"b", "wait", "{}"}, {"c", "wait", "{}"}};
        }
        return response;
    }
    void chat_full_streaming(const std::vector<Message>& messages, const json& tools,
        const TokenCallback& token, const std::function<void(LLMResponse)>& complete) override {
        auto response = chat_full(messages, tools);
        if (!response.content.empty()) {
            token(response.content);
            std::this_thread::sleep_for(2ms);
        }
        complete(response);
    }
    std::string name() const override { return "TimedLLM (synthetic)"; }
};
int main(int argc, char** argv) {
    int repeats = argc > 1 ? std::stoi(argv[1]) : 20;
    if (repeats < 1 || repeats > 1000) return 1;
    auto output = std::filesystem::path("logs/performance") /
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(output);
    TimedLLM llm;
    ToolRegistry registry;
    registry.register_tool(std::make_unique<WaitTool>());
    for (bool parallel : {false, true}) {
        ReActLoop agent(llm, registry, {});
        agent.set_parallel_tools(parallel);
        for (int i = 0; i < repeats; ++i) {
            auto path = output / (std::string(parallel ? "parallel-" : "serial-") + std::to_string(i) + ".json");
            TraceRecorder recorder(path.string());
            recorder.set_query("Run three independent 10ms tools.");
            agent.set_trace_callback(recorder.wrap({}));
            agent.run_streaming("Run three independent 10ms tools.", [](const auto&) {});
            if (!recorder.save_to_file()) return 1;
        }
    }
    fmt::print("Synthetic baseline: {} runs per mode, traces: {}\n", repeats, output.string());
}
