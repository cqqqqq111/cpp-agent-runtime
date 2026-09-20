/**
 * @file vector_index.h
 * @brief Brute-force cosine-similarity vector index for semantic memory.
 *
 * Day 9: Simple exhaustive search — O(n * d) per query. Sufficient for
 * up to ~10K vectors with < 50ms latency. Upgrade path: HNSW (hnswlib)
 * for sub-millisecond search on 1M+ vectors.
 *
 * Design:
 *   - Stores float vectors (embeddings) + arbitrary string metadata
 *   - Cosine similarity via dot product on L2-normalized vectors
 *   - No external dependencies (pure C++20)
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cpp_agent {

class VectorIndex {
public:
    struct Entry {
        std::vector<float> vector;
        std::string metadata;  // arbitrary JSON or text
    };

    /// Result of a similarity search.
    struct SearchResult {
        float similarity = 0.0f;  // 1.0 = identical, -1.0 = opposite
        std::string metadata;
    };

    // ---- API ----------------------------------------------------------------

    /// Add a vector with associated metadata.
    void add(std::vector<float> vec, std::string metadata);

    /// Search top-k most similar vectors using cosine similarity.
    /// @param query  The query vector (must be same dim as stored vectors).
    /// @param k      Number of results to return.
    /// @returns Sorted by similarity descending.
    std::vector<SearchResult> search(const std::vector<float>& query, size_t k = 5) const;

    /// Number of stored vectors.
    size_t size() const noexcept { return entries_.size(); }

    /// Dimensionality of stored vectors (0 if empty).
    size_t dimension() const noexcept { return dim_; }

    /// Remove all entries.
    void clear();

    /// Estimated memory usage in bytes.
    size_t estimated_bytes() const noexcept;

    // ---- Persistence (Day 22) ----------------------------------------------
    /// Serialize the index to a JSON file. Format:
    ///   {"dim": d, "entries": [{"vector": [...], "metadata": "..."}, ...]}
    /// @throws std::runtime_error if the file cannot be written.
    void save(const std::string& path) const;

    /// Load the index from a JSON file previously written by save().
    /// Replaces the current contents. @throws std::runtime_error on IO/parse error.
    void load(const std::string& path);

private:
    std::vector<Entry> entries_;
    size_t dim_ = 0;

    /// Cosine similarity: dot(A, B) assuming both are L2-normalized.
    /// OpenAI/Claude embeddings ARE normalized, so we can use raw dot product.
    static float cosine_similarity(const std::vector<float>& a,
                                   const std::vector<float>& b);
};

// ============================================================================
// Inline implementations
// ============================================================================

inline void VectorIndex::add(std::vector<float> vec, std::string metadata) {
    if (entries_.empty()) {
        dim_ = vec.size();
    }
    entries_.push_back(Entry{std::move(vec), std::move(metadata)});
}

inline std::vector<VectorIndex::SearchResult> VectorIndex::search(
    const std::vector<float>& query, size_t k) const {
    if (entries_.empty() || k == 0) return {};

    // Compute similarity for all entries
    std::vector<SearchResult> results;
    results.reserve(entries_.size());

    for (const auto& entry : entries_) {
        float sim = cosine_similarity(query, entry.vector);
        results.push_back(SearchResult{sim, entry.metadata});
    }

    // Partial sort: top-k by similarity descending
    k = std::min(k, results.size());
    std::partial_sort(
        results.begin(), results.begin() + static_cast<ptrdiff_t>(k), results.end(),
        [](const auto& a, const auto& b) { return a.similarity > b.similarity; });

    results.resize(k);
    return results;
}

inline void VectorIndex::clear() {
    entries_.clear();
    dim_ = 0;
}

inline size_t VectorIndex::estimated_bytes() const noexcept {
    size_t total = 0;
    for (const auto& e : entries_) {
        total += e.vector.size() * sizeof(float) + e.metadata.capacity() + sizeof(Entry);
    }
    return total;
}

inline void VectorIndex::save(const std::string& path) const {
    nlohmann::json j;
    j["dim"] = dim_;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries_) {
        arr.push_back({{"vector", e.vector}, {"metadata", e.metadata}});
    }
    j["entries"] = std::move(arr);

    std::ofstream f(path);
    if (!f) {
        throw std::runtime_error("VectorIndex::save: cannot open " + path);
    }
    f << j.dump(2);
}

inline void VectorIndex::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("VectorIndex::load: cannot open " + path);
    }
    nlohmann::json j = nlohmann::json::parse(f);

    clear();
    dim_ = j.value("dim", size_t(0));
    for (const auto& e : j.at("entries")) {
        entries_.push_back(Entry{
            e.at("vector").get<std::vector<float>>(),
            e.at("metadata").get<std::string>(),
        });
    }
}

inline float VectorIndex::cosine_similarity(
    const std::vector<float>& a, const std::vector<float>& b) {
    // OpenAI text-embedding-3-small returns normalized vectors.
    // For normalized vectors, cosine_sim = dot(A,B).
    // We still compute properly for non-normalized inputs.
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    size_t n = std::min(a.size(), b.size());

    for (size_t i = 0; i < n; ++i) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    if (denom < 1e-9f) return 0.0f;
    return dot / denom;
}

}  // namespace cpp_agent
