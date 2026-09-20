/**
 * Day 8 tests: WorkingMemory — set/get/TTL/context injection.
 */

#include "memory/long_term_memory.h"
#include "memory/vector_index.h"
#include "memory/working_memory.h"

#include <cassert>
#include <iostream>
#include <random>
#include <thread>

using namespace cpp_agent;

// ============================================================================
// Test 1: Basic set and get
// ============================================================================
static bool test_basic_set_get() {
    WorkingMemory wm;
    wm.set("user_name", "Alice");
    wm.set("topic", "code review");

    assert(wm.size() == 2);
    assert(wm.get("user_name") == "Alice");
    assert(wm.get("topic") == "code review");
    assert(!wm.get("nonexistent").has_value());

    return true;
}

// ============================================================================
// Test 2: Overwrite
// ============================================================================
static bool test_overwrite() {
    WorkingMemory wm;
    wm.set("x", "first");
    wm.set("x", "second");

    assert(wm.size() == 1);
    assert(wm.get("x") == "second");

    return true;
}

// ============================================================================
// Test 3: TTL expiration
// ============================================================================
static bool test_ttl_expiration() {
    WorkingMemory wm;

    // Set with 1-second TTL
    wm.set("ephemeral", "gone soon", std::chrono::seconds(1));
    assert(wm.contains("ephemeral"));

    // Wait for expiration
    std::this_thread::sleep_for(std::chrono::seconds(2));

    assert(!wm.contains("ephemeral"));
    assert(wm.get("ephemeral") == std::nullopt);
    // After get() on expired key, it should be removed
    assert(wm.size() == 0);

    return true;
}

// ============================================================================
// Test 4: Never-expires entry (ttl=0)
// ============================================================================
static bool test_never_expires() {
    WorkingMemory wm;
    wm.set("permanent", "always here");  // ttl=0 → never expires
    wm.set("temp", "short", std::chrono::seconds(1));

    // After 2 seconds
    std::this_thread::sleep_for(std::chrono::seconds(2));

    assert(wm.contains("permanent"));
    assert(!wm.contains("temp"));

    return true;
}

// ============================================================================
// Test 5: Purge expired
// ============================================================================
static bool test_purge_expired() {
    WorkingMemory wm;
    wm.set("a", "1");
    wm.set("b", "2", std::chrono::seconds(1));
    wm.set("c", "3", std::chrono::seconds(1));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Before purge: 3 entries still in map (2 expired but not yet cleaned)
    // Note: do NOT call contains()/get() here — they auto-clean expired entries
    assert(wm.size() == 3);

    // After purge: only 1 non-expired remains
    size_t removed = wm.purge_expired();
    assert(removed == 2);
    assert(wm.size() == 1);
    assert(wm.get("a") == "1");

    return true;
}

// ============================================================================
// Test 6: Forget and clear
// ============================================================================
static bool test_forget_and_clear() {
    WorkingMemory wm;
    wm.set("x", "1");
    wm.set("y", "2");
    assert(wm.size() == 2);

    wm.forget("x");
    assert(wm.size() == 1);
    assert(!wm.contains("x"));
    assert(wm.contains("y"));

    wm.clear();
    assert(wm.size() == 0);
    assert(!wm.contains("y"));

    return true;
}

// ============================================================================
// Test 7: Context string formatting
// ============================================================================
static bool test_context_string() {
    WorkingMemory wm;
    std::string empty = wm.to_context_string();
    assert(empty.empty());  // no entries yet

    wm.set("user_name", "Zhang San");
    wm.set("preferred_language", "C++");

    std::string ctx = wm.to_context_string();
    assert(ctx.find("[Working Memory]") != std::string::npos);
    assert(ctx.find("user_name = Zhang San") != std::string::npos);
    assert(ctx.find("preferred_language = C++") != std::string::npos);

    return true;
}

// ============================================================================
// Test 8: Active entries filtering
// ============================================================================
static bool test_active_entries() {
    WorkingMemory wm;
    wm.set("active1", "value1");
    wm.set("expired1", "old", std::chrono::seconds(1));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto active = wm.active_entries();
    assert(active.size() == 1);
    assert(active[0].key == "active1");

    return true;
}

// ============================================================================
// Test 9: Memory estimation < 5MB for 1000 entries
// ============================================================================
static bool test_memory_under_5mb() {
    WorkingMemory wm;
    for (int i = 0; i < 1000; ++i) {
        wm.set("key_" + std::to_string(i),
               "value_is_moderately_long_" + std::to_string(i));
    }
    size_t bytes = wm.estimated_bytes();

    std::cout << "  1000 entries → " << bytes << " bytes ("
              << (bytes / 1024.0) << " KB)\n";

    // Must be under 5 MB
    assert(bytes < 5 * 1024 * 1024);
    return true;
}

// ============================================================================
// Mock embedding: deterministic "pseudo-embedding" for testing
// ============================================================================
static std::vector<float> mock_embed(const std::string& text, size_t dim = 64) {
    std::vector<float> vec(dim, 0.0f);
    // Simple: hash each word and use it to seed the vector
    std::hash<std::string> hasher;

    // Use the hash to fill the vector deterministically
    for (size_t i = 0; i < dim; ++i) {
        size_t h = hasher(text + std::to_string(i));
        // Map to [-1, 1]
        vec[i] = (static_cast<float>(h % 2000) / 1000.0f) - 1.0f;
    }

    // L2-normalize
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 1e-9f) {
        for (float& v : vec) v /= norm;
    }
    return vec;
}

static EmbeddingFn make_mock_embedder(size_t dim = 64) {
    return [dim](const std::string& text) { return mock_embed(text, dim); };
}

// ============================================================================
// Test 10: VectorIndex — basic add and search
// ============================================================================
static bool test_vector_index_basic() {
    VectorIndex idx;

    // Add 5 vectors with distinct metadata
    idx.add(mock_embed("python programming"), "Python is great for AI");
    idx.add(mock_embed("c++ programming"), "C++ is fast");
    idx.add(mock_embed("cooking recipes"), "How to make pasta");
    idx.add(mock_embed("machine learning"), "Deep neural networks");
    idx.add(mock_embed("travel tips"), "Best places to visit in Europe");

    assert(idx.size() == 5);
    assert(idx.dimension() == 64);

    // Search for "programming"
    auto results = idx.search(mock_embed("coding and programming"), 3);
    assert(results.size() == 3);
    assert(!results[0].metadata.empty());
    // Similarity is always in [-1, 1]
    assert(results[0].similarity >= -1.0f && results[0].similarity <= 1.0f);

    return true;
}

// ============================================================================
// Test 11: VectorIndex — search latency < 50ms for 100 entries
// ============================================================================
static bool test_vector_index_latency() {
    VectorIndex idx;
    for (int i = 0; i < 100; ++i) {
        idx.add(mock_embed("document number " + std::to_string(i)),
                "doc_" + std::to_string(i));
    }
    assert(idx.size() == 100);

    auto t0 = std::chrono::steady_clock::now();
    auto results = idx.search(mock_embed("document number 42"), 5);
    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    std::cout << "  100 vectors, top-5 search → " << elapsed << " ms\n";
    assert(elapsed < 50.0);
    assert(results.size() == 5);

    return true;
}

// ============================================================================
// Test 12: LongTermMemory — remember and recall
// ============================================================================
static bool test_ltm_remember_recall() {
    LongTermMemory ltm(make_mock_embedder(64), 64);

    ltm.remember("User Zhang San prefers C++ for backend development");
    ltm.remember("Project is a high-performance AI agent runtime");
    ltm.remember("The codebase uses coroutine-based scheduling");
    ltm.remember("User likes pizza and travels frequently");

    assert(ltm.size() == 4);

    // Recall: "What programming languages does the user like?"
    auto results = ltm.recall("programming language preference", 3);
    assert(results.size() == 3);
    // With mock embeddings, we can't guarantee semantic ordering.
    // Just verify results are returned non-empty and have similarity scores.
    assert(results[0].similarity >= -1.0f && results[0].similarity <= 1.0f);
    assert(!results[0].metadata.empty());

    return true;
}

// ============================================================================
// Test 13: LongTermMemory — recall empty
// ============================================================================
static bool test_ltm_empty() {
    LongTermMemory ltm(make_mock_embedder(64), 64);
    auto results = ltm.recall("anything", 5);
    assert(results.empty());
    return true;
}

// ============================================================================
// Test 14: VectorIndex — large batch benchmark
// ============================================================================
static bool test_vector_index_1000() {
    VectorIndex idx;
    for (int i = 0; i < 1000; ++i) {
        idx.add(mock_embed("entry_" + std::to_string(i) + "_data"),
                "meta_" + std::to_string(i));
    }
    assert(idx.size() == 1000);

    auto t0 = std::chrono::steady_clock::now();
    auto results = idx.search(mock_embed("entry_500_data"), 10);
    auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();

    size_t bytes = idx.estimated_bytes();
    std::cout << "  1000 vectors (64d), top-10 → " << elapsed << " ms, "
              << (bytes / 1024.0) << " KB\n";

    assert(elapsed < 100.0);  // still well under 100ms for 1000
    assert(results.size() == 10);

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
        // WorkingMemory (Day 8)
        {"basic set/get", test_basic_set_get},
        {"overwrite same key", test_overwrite},
        {"TTL expiration", test_ttl_expiration},
        {"never-expires (ttl=0)", test_never_expires},
        {"purge expired entries", test_purge_expired},
        {"forget and clear", test_forget_and_clear},
        {"context string formatting", test_context_string},
        {"active entries filtering", test_active_entries},
        {"1000 entries < 5MB", test_memory_under_5mb},
        // LongTermMemory (Day 9)
        {"vector index basic add/search", test_vector_index_basic},
        {"vector index search < 50ms", test_vector_index_latency},
        {"LTM remember and recall", test_ltm_remember_recall},
        {"LTM empty recall", test_ltm_empty},
        {"vector index 1000 entries < 100ms", test_vector_index_1000},
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
