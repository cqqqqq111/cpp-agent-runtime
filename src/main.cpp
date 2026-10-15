#include <fmt/core.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "llm/llm_adapter.h"
#include "llm/py_bridge.h"
#include "memory/long_term_memory.h"
#include "memory/working_memory.h"
#include "tool/code_exec_tool.h"
#include "tool/echo_tool.h"
#include "tool/fs_tool.h"
#include "tool/git_tool.h"
#include "tool/grep_tool.h"
#include "tool/tool_base.h"
#include "tracing/trace_recorder.h"
#include "tui/terminal_ui.h"

using namespace cpp_agent;

// ============================================================================
// Demo agent (mock LLM — works without API key, just demos the TUI)
// ============================================================================
class DemoLLM : public LLMAdapter {
public:
    LLMResponse chat(const std::string&, const std::string& user) override {
        LLMResponse r;
        r.content = "[mock] You said: " + user;
        r.finish_reason = "stop";
        r.total_tokens = 10;
        return r;
    }
    LLMResponse chat_full(const std::vector<Message>&, const json& tools) override {
        LLMResponse r;
        // If tools are available, occasionally return a tool call
        if (!tools.empty() && tools.is_array() && tools.size() > 0 && call_count_++ == 0) {
            r.has_tool_calls = true;
            ToolCall tc;
            tc.id = "demo_1";
            tc.function_name = "echo";
            tc.arguments = R"({"message": "hello from TUI"})";
            r.tool_calls = {tc};
            r.finish_reason = "tool_calls";
        } else {
            r.content = "[mock agent] Received your message. Tools available: "
                      + std::to_string(tools.is_array() ? tools.size() : 0);
            r.finish_reason = "stop";
        }
        r.total_tokens = 20;
        return r;
    }
    std::string name() const override { return "DemoLLM"; }
private:
    int call_count_ = 0;
};

// ============================================================================
int main(int argc, char* argv[]) {
    // ── Logging ───────────────────────────────────────────────────────────
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/cpp-agent.log", true);
    auto logger = std::make_shared<spdlog::logger>(
        "cpp-agent", spdlog::sinks_init_list{console_sink, file_sink});
    logger->set_level(spdlog::level::warn);  // quieter for TUI
    logger->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    spdlog::set_default_logger(logger);

    // ── Parse args ────────────────────────────────────────────────────────
    struct BridgeCleanup { ~BridgeCleanup() { shutdown_python_bridge(); } } bridge_cleanup;
    bool tui_mode = false;
    bool offline = false;
    bool streaming = false;
    std::string tool_root = ".";
    std::string tool_names = "echo,filesystem,grep";
    bool tools_selected = false;
    std::string query = "Verify the agent tool pipeline.";
    std::string trace_file;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tui") == 0) tui_mode = true;
        else if (std::strcmp(argv[i], "--offline") == 0) offline = true;
        else if (std::strcmp(argv[i], "--stream") == 0) streaming = true;
        else if (std::strcmp(argv[i], "--tools") == 0) {
            if (i + 1 >= argc) { fmt::print(stderr, "--tools requires a comma-separated list\n"); return 1; }
            tool_names = argv[++i];
            tools_selected = true;
        }
        else if (std::strcmp(argv[i], "--tool-root") == 0 && i + 1 < argc) tool_root = argv[++i];
        else if (std::strcmp(argv[i], "--query") == 0 && i + 1 < argc) query = argv[++i];
        else if (std::strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_file = argv[++i];
        }
    }

    std::set<std::string> selected_tools;
    std::istringstream tool_list(tool_names);
    std::string tool_name;
    while (std::getline(tool_list, tool_name, ',')) {
        if (tool_name != "echo" && tool_name != "filesystem" && tool_name != "grep") {
            fmt::print(stderr, "Unknown tool: {}\n", tool_name);
            return 1;
        }
        selected_tools.insert(tool_name);
    }
    if (selected_tools.empty() || tool_names.back() == ',') {
        fmt::print(stderr, "--tools must name at least one tool\n"); return 1;
    }
    if (tools_selected && (offline || tui_mode)) {
        fmt::print(stderr, "--tools is supported in headless real-agent mode only\n"); return 1;
    }
    // ── TUI Mode ──────────────────────────────────────────────────────────
    if (offline && !tui_mode) {
        DemoLLM llm;
        ToolRegistry registry;
        registry.register_tool(std::make_unique<EchoTool>());
        AgentConfig config;
        ReActLoop agent(llm, registry, config);
        TraceRecorder recorder(trace_file);
        recorder.set_config(config);
        recorder.set_query(query);
        agent.set_trace_callback(recorder.wrap({}));
        auto started = std::chrono::steady_clock::now();
        auto answer = agent.run(query);
        recorder.finalize(agent.last_run_iterations(), agent.last_run_tokens(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), answer);
        recorder.save_to_file();
        fmt::print("Offline mode (scripted LLM, real echo tool)\n{}\n", answer);
        return 0;
    }
    if (tui_mode) {
        fmt::print("Starting cpp-agent-runtime TUI mode...\n");

        // ── Tools ──────────────────────────────────────────────────────────
        ToolRegistry registry;
        registry.register_tool(std::make_unique<EchoTool>());
        registry.register_tool(std::make_unique<FileSystemTool>());
        registry.register_tool(std::make_unique<GrepTool>());
        registry.register_tool(std::make_unique<CodeExecTool>());
        registry.register_tool(std::make_unique<GitTool>());

        AgentConfig config;
        config.system_prompt = "You are a coding assistant. Use tools when helpful.";
        config.max_iterations = 5;

        // ── Day 17 Task 1: real LLM when a key is configured ──────────────
        // The .env file is loaded on the Python side, so we ask Python whether
        // a key is present rather than reading process env vars directly.
        std::unique_ptr<LLMAdapter> llm;
        if (!offline && init_python_bridge() == 0) {
            try {
                json cfg = json::parse(llm_config_bridge());
                if (cfg.value("configured", false)) {
                    config.model = cfg.value("model", config.model);
                    llm = make_python_llm_adapter(config.model);
                }
            } catch (const std::exception& e) {
                spdlog::warn("LLM config check failed: {}", e.what());
            }
        } else {
            spdlog::warn("Python bridge unavailable — falling back to DemoLLM.");
        }
        if (!llm) {
            llm = std::make_unique<DemoLLM>();
        }
        fmt::print("LLM backend: {}\n", llm->name());

        // ── Day 17 Task 2: WorkingMemory (cross-turn scratchpad) ───────────
        WorkingMemory wm;

        // ── Day 17 Task 3: LongTermMemory (semantic recall, best-effort) ──
        // DeepSeek has no embeddings endpoint, so this stays inert until the
        // provider supports embeddings. Failures degrade to a no-op, not a crash.
        auto embed_fn = [](const std::string& text) -> std::vector<float> {
            std::string raw = call_embedding_bridge(text);   // JSON array string
            return json::parse(raw).get<std::vector<float>>();
        };
        LongTermMemory ltm(embed_fn, 1536);
        bool ltm_available = true;

        ReActLoop agent(*llm, registry, config);
        agent.set_working_memory(&wm);

        // Day 16: trace recorder (activated by --trace flag)
        TraceRecorder recorder(trace_file);
        recorder.set_config(config);

        // Wire trace callback to TUI log (chain via recorder)
        TerminalUI tui;
        // Day 18: TUI runs the agent in streaming mode (typewriter effect).
        constexpr bool kStreaming = true;

        auto tui_cb = [&](const TraceStep& ts) {
            switch (ts.phase) {
                case TraceStep::Phase::FINISH:
                    break;
                case TraceStep::Phase::THINK:
                    // In streaming mode the typewriter (append_to_last) paints
                    // the final text, so here we only surface tool-call intents.
                    if (!ts.tool_calls.empty()) {
                        tui.log(LogEntry::Kind::THINK,
                                "LLM wants to call " + std::to_string(ts.tool_calls.size()) + " tool(s)");
                    } else if (!kStreaming) {
                        tui.log(LogEntry::Kind::THINK,
                                "LLM: " + ts.llm_content.substr(0, 120));
                    }
                    break;
                case TraceStep::Phase::ACT:
                    for (const auto& tc : ts.tool_calls) {
                        tui.log(LogEntry::Kind::ACT,
                                "Execute: " + tc.function_name + "(" + tc.arguments + ")");
                    }
                    break;
                case TraceStep::Phase::OBSERVE:
                    tui.log(LogEntry::Kind::OBSERVE,
                            std::to_string(ts.tool_results.size()) + " tool result(s) received");
                    // Day 17 Task 2: auto-summarize large tool results into WM
                    for (const auto& tr : ts.tool_results) {
                        if (tr.content.size() > 500) {
                            wm.set("tool:" + tr.name, tr.content.substr(0, 200),
                                   std::chrono::seconds(300));
                        }
                    }
                    break;
            }
        };
        agent.set_trace_callback(recorder.wrap(tui_cb));

        tui.run([&](const std::string& input) -> std::string {
            // Day 17 Task 2: remember the query for cross-turn context
            wm.set("last_query", input);

            // Day 17 Task 3: recall relevant long-term memory (best-effort)
            std::string augmented = input;
            if (ltm_available && ltm.size() > 0) {
                try {
                    auto hits = ltm.recall(input, 3);
                    if (!hits.empty()) {
                        std::string ctx = "Relevant memories from earlier sessions:\n";
                        for (const auto& h : hits) ctx += "  - " + h.metadata + "\n";
                        augmented = ctx + "\n" + input;
                    }
                } catch (const std::exception& e) {
                    ltm_available = false;
                    spdlog::warn("LTM recall unavailable (disabling): {}", e.what());
                }
            }

            recorder.set_query(input);
            auto t0 = std::chrono::steady_clock::now();
            std::string result = agent.run_streaming(augmented, [&](const std::string& token) {
                // Day 18 Task 3: typewriter effect — append each token to the
                // live THINK line as it streams in.
                tui.append_to_last(LogEntry::Kind::THINK, token);
            });
            auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            recorder.finalize(agent.last_run_iterations(),
                              agent.last_run_tokens(),
                              elapsed, result);
            recorder.save_to_file();

            // Day 17 Task 3: store this exchange for future recall (best-effort)
            if (ltm_available) {
                try {
                    ltm.remember(input);
                } catch (const std::exception& e) {
                    ltm_available = false;
                    spdlog::warn("LTM store unavailable (disabling): {}", e.what());
                }
            }

            return result;
        });

        return 0;
    }

    // ── Headless real agent ──────────────────────────────────────────────
    fmt::print(R"(
╔══════════════════════════════════════════════════════════════╗
║              cpp-agent-runtime  v0.1.0                       ║
║    High-performance AI Agent engine | C++20 + pybind11       ║
╚══════════════════════════════════════════════════════════════╝
)");
    spdlog::info("Starting cpp-agent-runtime v0.1.0");

    spdlog::info("Initializing Python interpreter...");
    if (init_python_bridge() != 0) {
        spdlog::critical("Failed to initialize Python bridge.");
        return 1;
    }
    spdlog::info("Python bridge OK.");
    if (!json::parse(llm_config_bridge()).value("configured", false)) {
        fmt::print(stderr, "No API key configured. Use --offline to run the local agent demo, "
                           "or configure OPENAI_API_KEY for a real model.\n");
        return 1;
    }

    try {
        AgentConfig config;
        config.model = json::parse(llm_config_bridge()).value("model", config.model);
        config.system_prompt = "You are a coding assistant. Use the read-only tools to inspect code "
            "when requested. Tool paths are relative to the configured tool root. Answer concisely.";
        config.max_iterations = 4;
        config.iteration_timeout_ms = 60000;
        auto llm = make_python_llm_adapter(config.model);
        ToolRegistry registry;
        if (selected_tools.contains("echo")) registry.register_tool(std::make_unique<EchoTool>());
        if (selected_tools.contains("filesystem")) registry.register_tool(std::make_unique<FileSystemTool>(tool_root));
        if (selected_tools.contains("grep")) registry.register_tool(std::make_unique<GrepTool>(tool_root));
        ReActLoop agent(*llm, registry, config);
        TraceRecorder recorder(trace_file);
        recorder.set_config(config);
        recorder.set_query(query);
        agent.set_trace_callback(recorder.wrap({}));
        fmt::print("LLM backend: {}\n", llm->name());
        auto answer = streaming ? agent.run_streaming(query, [](const std::string& text) {
            fmt::print("{}", text);
            std::fflush(stdout);
        }) : agent.run(query);
        if (!streaming) fmt::print("{}", answer);
        fmt::print("\n");
        if (!recorder.save_to_file()) return 1;
        const auto& metrics = agent.last_run_metrics();
        fmt::print("Status: {} | LLM calls: {} | Tools: {} | Elapsed: {:.1f} ms\n",
            metrics.status, metrics.llm_calls, metrics.tool_calls, metrics.elapsed_ms);
        return metrics.status == "completed" ? 0 : 1;
    } catch (const std::exception& e) {
        spdlog::error("Agent failed: {}", e.what());
        return 1;
    }
}
