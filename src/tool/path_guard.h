#pragma once
#include <filesystem>
#include <optional>

namespace cpp_agent {
inline std::optional<std::filesystem::path> resolve_in_root(
    const std::filesystem::path& root, const std::filesystem::path& input) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto canonical_root = fs::weakly_canonical(fs::absolute(root), error);
    if (error) return std::nullopt;
    const auto candidate = fs::weakly_canonical(input.is_absolute() ? input : canonical_root / input, error);
    if (error) return std::nullopt;
    auto current = candidate.begin();
    for (const auto& component : canonical_root) {
        if (current == candidate.end() || *current != component) return std::nullopt;
        ++current;
    }
    return candidate;
}
}
