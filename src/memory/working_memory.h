/**
 * @file working_memory.h
 * @brief WorkingMemory — short-term key-value store with TTL expiration.
 *
 * Working memory is the agent's "scratchpad" within a session. It stores
 * facts the agent discovers (user's name, current topic, intermediate
 * results) and surfaces them in subsequent LLM calls via system-prompt
 * injection.
 *
 * Key design decisions:
 *   - std::unordered_map for O(1) key lookup
 *   - TTL per entry with lazy expiration (checked on access)
 *   - Thread-safe: no (Day 8 is single-threaded; mutex added Day 11)
 *   - Formatted context injection for system prompts
 *
 * Lifecycle analogy:
 *   WorkingMemory  ≈ scratchpad (this session)
 *   LongTermMemory ≈ vector DB (across sessions, Day 9)
 */

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

namespace cpp_agent {

class WorkingMemory {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // ---- Entry --------------------------------------------------------------
    struct Entry {
        std::string key;
        std::string value;
        TimePoint created_at;
        TimePoint expires_at;  // created_at + ttl

        bool is_expired() const noexcept {
            return Clock::now() >= expires_at;
        }
    };

    // ---- API ----------------------------------------------------------------

    WorkingMemory() = default;

    /// Store a value with a TTL (in seconds). 0 = never expires.
    void set(const std::string& key, const std::string& value,
             std::chrono::seconds ttl = std::chrono::seconds(0));

    /// Retrieve a value. Returns nullopt if not found or expired.
    /// Expired entries are removed on access (lazy cleanup).
    std::optional<std::string> get(const std::string& key);

    /// Check if a key exists and is not expired.
    bool contains(const std::string& key);

    /// Remove a key (even if expired).
    void forget(const std::string& key);

    /// Remove all entries (including non-expired).
    void clear();

    /// Number of entries (including expired ones not yet cleaned up).
    size_t size() const noexcept { return entries_.size(); }

    /// Remove all expired entries. Call periodically or before serialization.
    size_t purge_expired();

    /// Return all active (non-expired) entries for context injection.
    std::vector<Entry> active_entries() const;

    // ---- Context injection --------------------------------------------------
    //
    // Converts working memory into a string suitable for prepending to
    // the LLM system prompt. Format:
    //
    //   [Working Memory]
    //   - user_name = Zhang San
    //   - current_task = reviewing code
    //   (empty if no active entries)
    std::string to_context_string() const;

    // ---- Memory usage estimation --------------------------------------------
    size_t estimated_bytes() const noexcept;

private:
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace cpp_agent
