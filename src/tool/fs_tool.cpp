#include "tool/fs_tool.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cpp_agent {

// ============================================================================
// Construction
// ============================================================================

FileSystemTool::FileSystemTool(fs::path allowed_root)
    : allowed_root_(fs::absolute(allowed_root).lexically_normal()) {
    spdlog::info("FileSystemTool: allowed_root = {}", allowed_root_.string());
}

// ============================================================================
// Tool interface
// ============================================================================

json FileSystemTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"enum", {"read_file", "list_dir", "search_content"}},
                {"description", "Which filesystem operation to perform."}
            }},
            {"path", {
                {"type", "string"},
                {"description", "File or directory path (relative or absolute)."}
            }},
            {"pattern", {
                {"type", "string"},
                {"description", "Regex pattern for search_content action."}
            }}
        }},
        {"required", json::array({"action"})}
    };
}

ToolResult FileSystemTool::execute(const json& args, const std::string& call_id) {
    std::string action = args.value("action", "");
    std::string user_path = args.value("path", ".");

    // Validate the path
    auto resolved = validate_path(user_path);
    if (!resolved) {
        ToolResult tr;
        tr.tool_call_id = call_id;
        tr.name = name();
        tr.content = fmt::format(
            "Error: path '{}' is outside the allowed directory '{}' or is invalid.",
            user_path, allowed_root_.string());
        tr.success = false;
        return tr;
    }

    // Dispatch
    if (action == "read_file") {
        return do_read_file(*resolved, call_id);
    } else if (action == "list_dir") {
        return do_list_dir(*resolved, call_id);
    } else if (action == "search_content") {
        std::string pattern = args.value("pattern", "");
        if (pattern.empty()) {
            ToolResult tr;
            tr.tool_call_id = call_id;
            tr.name = name();
            tr.content = "Error: 'pattern' is required for search_content action.";
            tr.success = false;
            return tr;
        }
        return do_search_content(*resolved, pattern, call_id);
    } else {
        ToolResult tr;
        tr.tool_call_id = call_id;
        tr.name = name();
        tr.content = fmt::format(
            "Error: unknown action '{}'. Valid actions: read_file, list_dir, search_content.",
            action);
        tr.success = false;
        return tr;
    }
}

// ============================================================================
// Path security
// ============================================================================

std::optional<fs::path> FileSystemTool::validate_path(
    const std::string& user_path) const {
    try {
        fs::path input(user_path);

        // If relative, prepend allowed_root
        fs::path full;
        if (input.is_absolute()) {
            full = input;
        } else {
            full = allowed_root_ / input;
        }

        // Resolve symlinks, "..", "." to canonical form.
        // weakly_canonical works even if the file doesn't exist yet.
        fs::path canonical = fs::weakly_canonical(full);

        // Check containment: canonical path must start with allowed_root
        auto [root_end, _] = std::mismatch(
            allowed_root_.begin(), allowed_root_.end(),
            canonical.begin(), canonical.end());

        if (root_end != allowed_root_.end()) {
            spdlog::warn("FileSystemTool: blocked path traversal attempt: '{}' "
                         "→ canonical '{}' (root='{}')",
                         user_path, canonical.string(), allowed_root_.string());
            return std::nullopt;
        }

        return canonical;
    } catch (const fs::filesystem_error& e) {
        spdlog::error("FileSystemTool: path error: {}", e.what());
        return std::nullopt;
    }
}

// ---- Helpers ----------------------------------------------------------------
namespace {
std::string human_readable_size(uintmax_t bytes) {
    const char* suffixes[] = {"B", "KB", "MB", "GB"};
    int idx = 0;
    double d = static_cast<double>(bytes);
    while (d >= 1024.0 && idx < 3) { d /= 1024.0; ++idx; }
    return fmt::format("{:.1f} {}", d, suffixes[idx]);
}
}  // namespace

// ============================================================================
// Action: read_file
// ============================================================================

ToolResult FileSystemTool::do_read_file(const fs::path& path,
                                         const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    if (!fs::exists(path)) {
        tr.content = fmt::format("Error: file not found: {}", path.string());
        tr.success = false;
        return tr;
    }
    if (!fs::is_regular_file(path)) {
        tr.content = fmt::format("Error: not a regular file: {}", path.string());
        tr.success = false;
        return tr;
    }

    // Read with a size cap (10 MB) to avoid blowing up memory
    constexpr size_t kMaxSize = 10 * 1024 * 1024;
    auto fsize = fs::file_size(path);
    if (fsize > kMaxSize) {
        tr.content = fmt::format("Error: file too large ({} bytes, max {})",
                                 fsize, kMaxSize);
        tr.success = false;
        return tr;
    }

    std::ifstream file(path, std::ios::in);
    if (!file) {
        tr.content = fmt::format("Error: cannot open file: {}", path.string());
        tr.success = false;
        return tr;
    }

    std::ostringstream out;
    out << "File: " << path.filename().string() << " (" << fsize << " bytes)\n";
    out << std::string(60, '-') << "\n";

    std::string line;
    int line_no = 0;
    while (std::getline(file, line)) {
        ++line_no;
        out << fmt::format("{:4d} │ {}\n", line_no, line);
    }

    tr.content = out.str();
    tr.success = true;

    spdlog::debug("FileSystemTool: read_file '{}' → {} lines, {} bytes",
                  path.string(), line_no, fsize);
    return tr;
}

// ============================================================================
// Action: list_dir
// ============================================================================

ToolResult FileSystemTool::do_list_dir(const fs::path& path,
                                        const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    if (!fs::exists(path)) {
        tr.content = fmt::format("Error: directory not found: {}", path.string());
        tr.success = false;
        return tr;
    }
    if (!fs::is_directory(path)) {
        tr.content = fmt::format("Error: not a directory: {}", path.string());
        tr.success = false;
        return tr;
    }

    std::ostringstream out;
    out << "Directory: " << path.string() << "\n";
    out << std::string(60, '-') << "\n";

    // Collect and sort entries
    std::vector<fs::directory_entry> entries;
    try {
        for (const auto& entry : fs::directory_iterator(path)) {
            entries.push_back(entry);
        }
    } catch (const fs::filesystem_error& e) {
        tr.content = fmt::format("Error reading directory: {}", e.what());
        tr.success = false;
        return tr;
    }

    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) {
                  // Directories first, then by name
                  bool da = a.is_directory(), db = b.is_directory();
                  if (da != db) return da > db;
                  return a.path().filename() < b.path().filename();
              });

    int file_count = 0, dir_count = 0;
    for (const auto& entry : entries) {
        if (entry.is_directory()) {
            out << fmt::format("  📁 {}/\n", entry.path().filename().string());
            ++dir_count;
        } else {
            auto sz = entry.is_regular_file() ? entry.file_size() : 0;
            out << fmt::format("  📄 {}  ({})\n",
                              entry.path().filename().string(),
                              human_readable_size(sz));
            ++file_count;
        }
    }

    out << std::string(60, '-') << "\n";
    out << fmt::format("{} directories, {} files\n", dir_count, file_count);

    tr.content = out.str();
    tr.success = true;
    return tr;
}

// ============================================================================
// Action: search_content
// ============================================================================

ToolResult FileSystemTool::do_search_content(const fs::path& path,
                                              const std::string& pattern,
                                              const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    if (!fs::exists(path)) {
        tr.content = fmt::format("Error: path not found: {}", path.string());
        tr.success = false;
        return tr;
    }

    // Compile regex
    std::regex re;
    try {
        re = std::regex(pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        tr.content = fmt::format("Error: invalid regex pattern '{}': {}", pattern, e.what());
        tr.success = false;
        return tr;
    }

    std::ostringstream out;
    out << fmt::format("Search: pattern='{}' in {}\n", pattern, path.string());
    out << std::string(60, '-') << "\n";

    int total_matches = 0;
    int files_searched = 0;
    constexpr int kMaxMatches = 200;
    constexpr int kMaxFileSize = 5 * 1024 * 1024;  // 5 MB per file

    // Helper: search one file
    auto search_file = [&](const fs::path& fp) {
        if (total_matches >= kMaxMatches) return;
        if (!fs::is_regular_file(fp)) return;
        if (fs::file_size(fp) > kMaxFileSize) return;

        std::ifstream file(fp);
        if (!file) return;

        std::string line;
        int line_no = 0;
        while (std::getline(file, line) && total_matches < kMaxMatches) {
            ++line_no;
            if (std::regex_search(line, re)) {
                out << fmt::format("{}:{:d}: {}\n",
                                  fs::relative(fp, allowed_root_).string(),
                                  line_no, line);
                ++total_matches;
            }
        }
        ++files_searched;
    };

    // If path is a directory, walk recursively; else search just that file
    if (fs::is_directory(path)) {
        constexpr int kMaxFiles = 1000;
        int file_count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(
                 path, fs::directory_options::skip_permission_denied)) {
            if (file_count >= kMaxFiles) break;
            if (entry.is_regular_file()) {
                search_file(entry.path());
                ++file_count;
            }
        }
    } else {
        search_file(path);
    }

    out << std::string(60, '-') << "\n";
    out << fmt::format("{} match(es) in {} file(s)\n", total_matches, files_searched);
    if (total_matches >= kMaxMatches) {
        out << "(results truncated at " << kMaxMatches << " matches)\n";
    }

    tr.content = out.str();
    tr.success = true;
    return tr;
}

}  // namespace cpp_agent
