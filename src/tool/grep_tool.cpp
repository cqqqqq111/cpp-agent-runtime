#include "tool/grep_tool.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace cpp_agent {

GrepTool::GrepTool(fs::path root) : root_(std::move(root)) {}

json GrepTool::parameters_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Regex pattern to search for."}
            }},
            {"path", {
                {"type", "string"},
                {"description", "File or directory to search (relative to project root)."}
            }},
            {"glob", {
                {"type", "string"},
                {"description", "Optional file filter, e.g. '*.cpp' or '*.h'."}
            }}
        }},
        {"required", json::array({"pattern"})}
    };
}

// Simple glob match: checks if filename matches a glob pattern like "*.cpp"
static bool glob_match(const std::string& filename, const std::string& pattern) {
    if (pattern == "*" || pattern == "*.*") return true;

    // Simple prefix/suffix matching for common patterns like *.cpp
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        std::string ext = pattern.substr(1);  // ".cpp"
        return filename.size() >= ext.size() &&
               filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0;
    }

    // Exact match
    return filename == pattern;
}

ToolResult GrepTool::execute(const json& args, const std::string& call_id) {
    ToolResult tr;
    tr.tool_call_id = call_id;
    tr.name = name();

    std::string pattern_str = args.value("pattern", "");
    if (pattern_str.empty()) {
        tr.content = "Error: 'pattern' is required.";
        tr.success = false;
        return tr;
    }

    // Compile regex
    std::regex re;
    try {
        re = std::regex(pattern_str, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        tr.content = fmt::format("Error: invalid regex: {}", e.what());
        tr.success = false;
        return tr;
    }

    // Resolve search path
    fs::path search_path = root_;
    std::string user_path = args.value("path", "");
    if (!user_path.empty()) {
        fs::path up(user_path);
        search_path = up.is_absolute() ? up : root_ / up;
    }

    if (!fs::exists(search_path)) {
        tr.content = fmt::format("Error: path not found: {}", search_path.string());
        tr.success = false;
        return tr;
    }

    std::string glob_filter = args.value("glob", "");

    std::ostringstream out;
    out << fmt::format("grep: pattern='{}' in {}\n", pattern_str, search_path.string());
    out << std::string(60, '-') << "\n";

    int total_matches = 0;
    int files_searched = 0;
    constexpr int kMaxMatches = 200;
    constexpr int kMaxFiles = 500;

    auto search_file = [&](const fs::path& fp) {
        if (total_matches >= kMaxMatches) return;
        if (!fs::is_regular_file(fp)) return;
        if (fs::file_size(fp) > 5 * 1024 * 1024) return;  // skip > 5MB

        std::ifstream file(fp);
        if (!file) return;

        std::string line;
        int line_no = 0;
        while (std::getline(file, line) && total_matches < kMaxMatches) {
            ++line_no;
            if (std::regex_search(line, re)) {
                auto rel = fs::relative(fp, root_).string();
                out << fmt::format("{}:{:d}: {}\n", rel, line_no, line);
                ++total_matches;
            }
        }
        ++files_searched;
    };

    if (fs::is_directory(search_path)) {
        int file_count = 0;
        for (const auto& entry : fs::recursive_directory_iterator(
                 search_path, fs::directory_options::skip_permission_denied)) {
            if (file_count >= kMaxFiles) break;
            if (entry.is_regular_file()) {
                if (!glob_filter.empty() &&
                    !glob_match(entry.path().filename().string(), glob_filter))
                    continue;
                search_file(entry.path());
                ++file_count;
            }
        }
    } else {
        search_file(search_path);
    }

    out << std::string(60, '-') << "\n";
    out << fmt::format("{} match(es) in {} file(s)\n", total_matches, files_searched);
    if (total_matches >= kMaxMatches)
        out << "(results truncated at " << kMaxMatches << " matches)\n";

    tr.content = out.str();
    tr.success = true;

    spdlog::debug("GrepTool: '{}' → {} matches in {} files",
                  pattern_str, total_matches, files_searched);
    return tr;
}

}  // namespace cpp_agent
