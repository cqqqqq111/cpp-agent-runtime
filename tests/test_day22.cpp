/**
 * Day 22 tests: persistence (VectorIndex / LongTermMemory save+load) and
 * per-iteration LLM timeout (ReActLoop).
 *
 *   persistence  — round-trip a vector index through a JSON file
 *   timeout      — a slow LLM is cut off mid-iteration instead of hanging
 */

#include "agent/agent_config.h"
#include "agent/react_loop.h"
#include "memory/long_term_memory.h"
#include "memory/vector_index.h"
#include "tool/echo_tool.h"
#include "tool/tool_base.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace cpp_agent;

// ============================================================================
// Deterministic pseudo-embedder (same as test_memory.cpp)
// ============================================================================
static std::vector<float> mock_embed(const std::string& text, size_t dim = 32) {
    std::vector<float> vec(dim, 0.0f);
    std::hash<std::string> hasher;
    for (size_t i = 0; i < dim; ++i) {
        size_t h = hasher(text + std::to_string(i));
        vec[i] = (static_cast<float>(h % 2000) / 1000.0f) - 1.0f;
    }
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-9f)
        for (float& v : vec) v /= norm;
    return vec;
}

static EmbeddingFn make_mock_embedder(size_t dim = 32) {
    return [dim](const std::string& t) { return mock_embed(t, dim); };
}

// ============================================================================
// Persistence tests
// ============================================================================

static bool test_vector_index_save_load() {
    VectorIndex idx;
    idx.add(mock_embed("c++ performance"), "C++ is fast");
    idx.add(mock_embed("python ai"), "Python for AI");
    idx.add(mock_embed("cooking"), "How to make pasta");

    const char* path = "test_vi_dump.json";
    idx.save(path);

    VectorIndex loaded;
    loaded.load(path);

    assert(loaded.size() == 3);
    assert(loaded.dimension() == 32);
    // Search must give identical top result metadata.
    auto before = idx.search(mock_embed("c++ programming"), 1);
    auto after  = loaded.search(mock_embed("c++ programming"), 1);
    assert(!before.empty() && !after.empty());
    assert(before[0].metadata == after[0].metadata);

    std::remove(path);
    return true;
}

static bool test_vector_index_save_empty() {
    VectorIndex idx;
    const char* path = "test_vi_empty.json";
    idx.save(path);

    VectorIndex loaded;
    loaded.load(path);
    assert(loaded.size() == 0);
    assert(loaded.dimension() == 0);

    std::remove(path);
    return true;
}

static bool test_vector_index_save_bad_path_throws() {
    VectorIndex idx;
    idx.add(mock_embed("x"), "y");
    bool threw = false;
    try {
        idx.save("no_such_dir_xyz/dump.json");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    return true;
}

static bool test_vector_index_load_missing_throws() {
    VectorIndex idx;
    bool threw = false;
    try {
        idx.load("does_not_exist_xyz.json");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
    return true;
}

static bool test_ltm_save_load() {
    LongTermMemory ltm(make_mock_embedder(32), 32);
    ltm.remember("User prefers C++ for backend");
    ltm.remember("Project is an AI agent runtime");

    const char* path = "test_ltm_dump.json";
    ltm.save(path);

    LongTermMemory ltm2(make_mock_embedder(32), 32);
    ltm2.load(path);
    assert(ltm2.size() == 2);
    auto res = ltm2.recall("programming language", 2);
    assert(res.size() == 2);

    std::remove(path);
    return true;
}

// ============================================================================
// Timeout tests
// ============================================================================

/// LLM adapter that sleeps a fixed duration on every call, then answers.
class SlowLLM : public LLMAdapter {
public:
    explicit SlowLLM(std::chrono::milliseconds delay, bool with_tool = false)
        : delay_(delay), with_tool_(with_tool) {}

    LLMResponse chat(const std::string&, const std::string&) override {
        std::this_thread::sleep_for(delay_);
        LLMResponse r;
        r.content = "done";
        r.finish_reason = "stop";
        r.total_tokens = 10;
        return r;
    }

    LLMResponse chat_full(const std::vector<Message>&, const json&) override {
        if (timeout_ms_ > 0 && delay_ > std::chrono::milliseconds(timeout_ms_)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms_));
            throw LLMTimeout("request timeout");
        }
        std::this_thread::sleep_for(delay_);
        LLMResponse r;
        if (with_tool_) {
            ToolCall tc;
            tc.id = "slow_1";
            tc.function_name = "echo";
            tc.arguments = R"({"message":"hi"})";
            r.has_tool_calls = true;
            r.tool_calls = {tc};
            r.finish_reason = "tool_calls";
        } else {
            r.content = "slow final answer";
            r.finish_reason = "stop";
        }
        r.total_tokens = 10;
        return r;
    }

    std::string name() const override { return "SlowLLM"; }
    void set_request_timeout(int milliseconds) override { timeout_ms_ = milliseconds; }

private:
    std::chrono::milliseconds delay_;
    bool with_tool_;
    int timeout_ms_ = 0;
};

static bool test_timeout_triggers() {
    // LLM sleeps 300ms; timeout fires at 40ms → loop must abort gracefully.
    SlowLLM llm(std::chrono::milliseconds(300));
    ToolRegistry registry;
    AgentConfig config;
    config.max_iterations = 5;
    config.iteration_timeout_ms = 40;

    ReActLoop agent(llm, registry, config);
    auto started = std::chrono::steady_clock::now();
    std::string result = agent.run("do something slow");

    assert(result.find("timeout") != std::string::npos);
    assert(agent.last_run_iterations() == 1);  // aborted during the first call

    assert(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(250));
    // Destruction is immediate: no detached worker or delayed callback survives.
    return true;
}

static bool test_timeout_disabled_completes() {
    // LLM sleeps 20ms; no timeout (0) → completes normally.
    SlowLLM llm(std::chrono::milliseconds(20));
    ToolRegistry registry;
    AgentConfig config;
    config.max_iterations = 5;
    config.iteration_timeout_ms = 0;  // disabled

    ReActLoop agent(llm, registry, config);
    std::string result = agent.run("fast enough");

    assert(result == "slow final answer");
    assert(agent.last_run_iterations() == 1);
    return true;
}

static bool test_timeout_not_triggered_when_fast() {
    // LLM sleeps 10ms; timeout is a generous 500ms → no abort.
    SlowLLM llm(std::chrono::milliseconds(10));
    ToolRegistry registry;
    AgentConfig config;
    config.max_iterations = 5;
    config.iteration_timeout_ms = 500;

    ReActLoop agent(llm, registry, config);
    std::string result = agent.run("fast");

    assert(result == "slow final answer");
    return true;
}

// ============================================================================
// Runner
// ============================================================================
int main() {
    struct {
        const char* name;
        bool (*fn)();
    } tests[] = {
        {"vector index save/load round-trip", test_vector_index_save_load},
        {"vector index save/load empty", test_vector_index_save_empty},
        {"vector index save bad path throws", test_vector_index_save_bad_path_throws},
        {"vector index load missing throws", test_vector_index_load_missing_throws},
        {"long-term memory save/load round-trip", test_ltm_save_load},
        {"iteration timeout triggers", test_timeout_triggers},
        {"iteration timeout disabled completes", test_timeout_disabled_completes},
        {"iteration timeout not triggered when fast", test_timeout_not_triggered_when_fast},
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
