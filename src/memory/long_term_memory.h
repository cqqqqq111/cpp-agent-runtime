/**
 * @file long_term_memory.h
 * @brief LongTermMemory — semantic search across conversations.
 *
 * Wraps VectorIndex + embedding bridge to provide:
 *   remember(text)     → embed → store in vector index
 *   recall(query, k)   → embed query → search top-k → return texts
 *
 * The embedding function is injected (from Python via pybind11),
 * keeping this class provider-agnostic.
 *
 * Day 9: brute-force cosine similarity (VectorIndex)
 * Day 17+: upgrade to HNSW index for scale
 */

#pragma once

#include "memory/vector_index.h"

#include <functional>
#include <string>
#include <vector>

namespace cpp_agent {

/// Signature: embed(text) → vector<float>
using EmbeddingFn = std::function<std::vector<float>(const std::string& text)>;

class LongTermMemory {
public:
    /// @param embed_fn  Function that converts text to an embedding vector.
    /// @param dim       Expected embedding dimension.
    explicit LongTermMemory(EmbeddingFn embed_fn, size_t dim = 1536);

    /// Store a text in long-term memory (embeds + indexes).
    void remember(const std::string& text);

    /// Search for top-k semantically similar texts.
    std::vector<VectorIndex::SearchResult> recall(const std::string& query, size_t k = 5);

    /// Number of stored texts.
    size_t size() const noexcept { return index_.size(); }

    /// Remove all entries.
    void clear() { index_.clear(); }

    /// Estimated memory usage.
    size_t estimated_bytes() const noexcept { return index_.estimated_bytes(); }

    /// Persist the underlying vector index to disk (Day 22). The embedding
    /// function is NOT serialized — the caller must re-inject it via the
    /// constructor before calling recall() again.
    void save(const std::string& path) const { index_.save(path); }

    /// Load the index from disk (Day 22). Replaces current contents.
    /// @throws std::runtime_error on IO/parse error.
    void load(const std::string& path) { index_.load(path); }

private:
    VectorIndex index_;
    EmbeddingFn embed_;
    size_t dim_;
};

// ============================================================================
// Inline implementations
// ============================================================================

inline LongTermMemory::LongTermMemory(EmbeddingFn embed_fn, size_t dim)
    : embed_(std::move(embed_fn)), dim_(dim) {}

inline void LongTermMemory::remember(const std::string& text) {
    if (!embed_) return;
    auto vec = embed_(text);
    index_.add(std::move(vec), text);
}

inline std::vector<VectorIndex::SearchResult> LongTermMemory::recall(
    const std::string& query, size_t k) {
    if (!embed_) return {};
    auto qvec = embed_(query);
    return index_.search(qvec, k);
}

}  // namespace cpp_agent
