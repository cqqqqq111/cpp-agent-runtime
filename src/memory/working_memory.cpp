#include "memory/working_memory.h"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace cpp_agent {

// ============================================================================
// API
// ============================================================================

void WorkingMemory::set(const std::string& key, const std::string& value,
                         std::chrono::seconds ttl) {
    auto now = Clock::now();
    auto expires = (ttl.count() == 0)
        ? TimePoint::max()          // never expires
        : now + ttl;

    entries_[key] = Entry{key, value, now, expires};

    spdlog::debug("WorkingMemory: set '{}' = '{}' (ttl={}s, total={})",
                  key, value, ttl.count(), entries_.size());
}

std::optional<std::string> WorkingMemory::get(const std::string& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }

    if (it->second.is_expired()) {
        spdlog::debug("WorkingMemory: '{}' expired, removing", key);
        entries_.erase(it);
        return std::nullopt;
    }

    return it->second.value;
}

bool WorkingMemory::contains(const std::string& key) {
    return get(key).has_value();
}

void WorkingMemory::forget(const std::string& key) {
    entries_.erase(key);
}

void WorkingMemory::clear() {
    entries_.clear();
}

size_t WorkingMemory::purge_expired() {
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end(); ) {
        if (it->second.is_expired()) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        spdlog::debug("WorkingMemory: purged {} expired entries, {} remain",
                      removed, entries_.size());
    }
    return removed;
}

std::vector<WorkingMemory::Entry> WorkingMemory::active_entries() const {
    std::vector<Entry> result;
    for (const auto& [_, entry] : entries_) {
        if (!entry.is_expired()) {
            result.push_back(entry);
        }
    }
    return result;
}

// ============================================================================
// Context injection
// ============================================================================

std::string WorkingMemory::to_context_string() const {
    auto active = active_entries();
    if (active.empty()) {
        return "";
    }

    std::string ctx = "[Working Memory]\n";
    ctx += "The following facts are stored in your working memory. ";
    ctx += "Use them to provide personalized and context-aware responses.\n\n";

    for (const auto& entry : active) {
        ctx += "- " + entry.key + " = " + entry.value + "\n";
    }

    return ctx;
}

// ============================================================================
// Memory estimation
// ============================================================================

size_t WorkingMemory::estimated_bytes() const noexcept {
    size_t total = 0;
    for (const auto& [key, entry] : entries_) {
        total += key.capacity() + entry.value.capacity() + sizeof(Entry);
    }
    return total;
}

}  // namespace cpp_agent
