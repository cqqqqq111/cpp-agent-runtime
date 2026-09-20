/**
 * @file fs_tool.h
 * @brief FileSystemTool — read, list, and search files on disk.
 *
 * Three operations exposed as a single tool (the LLM selects which
 * via the "action" parameter):
 *
 *   read_file(path)      — return file contents with line numbers
 *   list_dir(path)       — list directory entries
 *   search_content(path, pattern) — regex search within files
 *
 * SECURITY: All paths are resolved to canonical form and validated
 * against an allowed root directory. Path traversal attacks
 * ("../../etc/passwd") are blocked.
 */

#pragma once

#include "tool/tool_base.h"

#include <filesystem>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace cpp_agent {

class FileSystemTool : public Tool {
public:
    /// @param allowed_root  All file access is restricted to this directory tree.
    ///                       Defaults to current working directory.
    explicit FileSystemTool(fs::path allowed_root = fs::current_path());

    // ---- Tool interface -----------------------------------------------------
    std::string name() const override { return "filesystem"; }

    std::string description() const override {
        return "Read files, list directories, or search file contents. "
               "Use the 'action' parameter to choose: 'read_file', 'list_dir', "
               "or 'search_content'. "
               "For 'read_file' provide 'path' (string). "
               "For 'list_dir' provide 'path' (string, default '.'). "
               "For 'search_content' provide 'path' (directory) and 'pattern' (regex).";
    }

    json parameters_schema() const override;

    ToolResult execute(const json& args, const std::string& call_id) override;

private:
    fs::path allowed_root_;

    // ---- Path security ------------------------------------------------------
    /// Resolve a user-supplied path, validate it's within allowed_root.
    /// Returns empty optional if the path is invalid or escapes the sandbox.
    std::optional<fs::path> validate_path(const std::string& user_path) const;

    // ---- Action implementations ---------------------------------------------
    ToolResult do_read_file(const fs::path& path, const std::string& call_id);
    ToolResult do_list_dir(const fs::path& path, const std::string& call_id);
    ToolResult do_search_content(const fs::path& path, const std::string& pattern,
                                 const std::string& call_id);
};

}  // namespace cpp_agent
