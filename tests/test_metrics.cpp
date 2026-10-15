#include "agent/react_loop.h"
#include "tracing/trace_recorder.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace cpp_agent;
using namespace std::chrono_literals;

class MeasuredTool : public Tool {
public:
    std::string name() const override { return "measured"; }
    std::string description() const override { return "test"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json& args, const std::string& id) override {
        std::this_thread::sleep_for(10ms);
        if (args.value("fail", false)) throw std::runtime_error("test failure");
        return {id, name(), "output", true};
    }
};

class MeasuredLLM : public LLMAdapter {
public:
    std::string failure;
    LLMResponse chat(const std::string&, const std::string&) override { return {}; }
    LLMResponse chat_full(const std::vector<Message>& messages, const json&) override {
        std::this_thread::sleep_for(5ms);
        if (failure == "timeout") throw LLMTimeout("test timeout");
        if (failure == "error") throw std::runtime_error("test error");
        LLMResponse response;
        if (messages.back().is_tool_result()) response.content = "done";
        else {
            response.has_tool_calls = true;
            response.tool_calls = {{"a", "measured", "{}"}, {"b", "measured", R"({"fail":true})"}};
        }
        return response;
    }
    void chat_full_streaming(const std::vector<Message>& messages, const json& tools,
        const TokenCallback& token, const std::function<void(LLMResponse)>& complete) override {
        auto response = chat_full(messages, tools);
        if (!response.content.empty()) {
            token(""); // Empty chunks must not create a first-text measurement.
            std::this_thread::sleep_for(5ms);
            token(response.content);
            std::this_thread::sleep_for(5ms);
        }
        complete(response);
    }
    std::string name() const override { return "MeasuredLLM"; }
};

int main() {
    MeasuredLLM llm;
    ToolRegistry registry;
    registry.register_tool(std::make_unique<MeasuredTool>());
    ReActLoop agent(llm, registry, {});
    auto output = std::filesystem::temp_directory_path() /
        ("cpp-agent-metrics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    TraceRecorder recorder(output.string());
    agent.set_trace_callback(recorder.wrap({}));

    for (bool parallel : {false, true}) {
        agent.set_parallel_tools(parallel);
        std::string tokens;
        assert(agent.run_streaming("hello", [&](const auto& token) { tokens += token; }) == "done");
        assert(tokens == "done");
        const auto& metrics = agent.last_run_metrics();
        assert(metrics.status == "completed" && metrics.llm_calls == 2 && metrics.tool_calls == 2);
        assert(metrics.input_bytes == 5 && metrics.output_bytes == 4);
        assert(metrics.llm_ms >= 20 && metrics.tool_execution_ms >= 20);
        assert(metrics.elapsed_ms + 0.001 >= metrics.llm_ms + metrics.tools_wall_ms);
        auto trace = recorder.to_json();
        assert(trace["steps"].size() == 4); // FINISH is a summary, not a timeline step.
        assert(trace["summary"]["performance"]["run_id"] == metrics.run_id);
        auto& first = trace["steps"][0]["request_metrics"];
        auto& last = trace["steps"][3]["request_metrics"];
        assert(first["first_text_ms"].is_null());
        assert(last["first_text_ms"].get<double>() >= 10);
        assert(last["duration_ms"].get<double>() >= last["first_text_ms"].get<double>());
        assert(first["input_content_bytes"].get<size_t>() >= 5);
        assert(last["output_content_bytes"] == 4);
        auto& tools = trace["steps"][2]["tool_metrics"];
        assert(tools.size() == 2 && tools[0]["call_id"] == "a" && tools[1]["call_id"] == "b");
        assert(tools[0]["success"] == true && tools[1]["success"] == false);
        assert(tools[0]["input_bytes"] == 2 && tools[0]["output_bytes"] == 6);
        if (!parallel) assert(tools[1]["queue_ms"].get<double>() >= tools[0]["execution_ms"].get<double>());
        double previous = 0;
        for (const auto& step : trace["steps"]) {
            auto offset = step["elapsed_ms"].get<double>();
            assert(offset >= previous);
            previous = offset;
        }
    }
    // Metrics are available without a trace subscriber; no stale TTFT from a prior stream.
    assert(agent.run("again") == "done");
    assert(recorder.to_json()["steps"][3]["request_metrics"]["first_text_ms"].is_null());
    llm.failure = "timeout";
    assert(agent.run("slow").find("timeout") != std::string::npos);
    auto trace = recorder.to_json();
    assert(trace["steps"].size() == 1);
    assert(trace["steps"][0]["request_metrics"]["status"] == "timeout");
    assert(agent.last_run_metrics().llm_ms >= 5 && agent.last_run_metrics().status == "timeout");
    llm.failure = "error";
    bool threw = false;
    try { agent.run("error"); } catch (const std::runtime_error& e) { threw = std::string(e.what()) == "test error"; }
    assert(threw && agent.last_run_metrics().status == "error");
    assert(std::filesystem::exists(output)); // Exception trace is saved automatically.
    assert(recorder.to_json()["summary"]["performance"]["status"] == "error");
    std::filesystem::remove(output);
    agent.set_trace_callback({});
    llm.failure.clear();
    assert(agent.run("without trace") == "done");
    assert(agent.last_run_metrics().tool_calls == 2);
    std::cout << "Serial/parallel, streaming, failure, timeout and repeat-run metrics passed\n";
}
