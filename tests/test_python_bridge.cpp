#include "agent/react_loop.h"
#include "llm/py_bridge.h"
#include "tool/echo_tool.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace cpp_agent;
int main() {
    if (init_python_bridge() != 0) return 1;
    int result = 0;
    // The TUI makes requests on a worker thread. Exercise that same GIL path.
    std::thread worker([&] {
        try {
            auto llm = make_python_llm_adapter("local-test-model");
            ToolRegistry registry;
            registry.register_tool(std::make_unique<EchoTool>());
            AgentConfig config;
            config.iteration_timeout_ms = 3000;
            ReActLoop agent(*llm, registry, config);
            assert(agent.run("full") == "tool pipeline complete");
            assert(agent.last_run_iterations() == 2);
            std::string tokens;
            assert(agent.run_streaming("stream", [&](const std::string& token) { tokens += token; }) == "tool pipeline complete");
            assert(tokens == "tool pipeline complete");
            assert(agent.last_run_tokens() == 26);
            assert(agent.last_run_iterations() == 2);
            config.iteration_timeout_ms = 200;
            assert(agent.last_run_metrics().llm_calls == 2);
            assert(agent.last_run_metrics().elapsed_ms >= agent.last_run_metrics().llm_ms);
            ReActLoop bounded(*llm, registry, config);
            for (bool streaming : {false, true}) {
                auto started = std::chrono::steady_clock::now();
                auto response = streaming ? bounded.run_streaming("slow", [](const auto&) {}) : bounded.run("slow");
                assert(response.find("timeout") != std::string::npos);
                assert(bounded.last_run_metrics().status == "timeout");
                assert(bounded.last_run_metrics().llm_ms >= 100);
                assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
            }
            assert(agent.run("after timeout") == "tool pipeline complete");
            // Immediately destroy the agent/adapter after timeout; no sleeps.
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            result = 1;
        }
    });
    worker.join();
    shutdown_python_bridge();
    // Shutdown is idempotent. Native SDK extensions do not support destroying
    // and recreating the embedded interpreter: reject that instead of crashing.
    shutdown_python_bridge();
    assert(init_python_bridge() != 0);
    return result;
}
