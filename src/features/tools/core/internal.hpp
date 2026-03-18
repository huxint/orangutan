#pragma once

#include "core/tools/tool.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace orangutan {

inline std::filesystem::path normalize_tool_path(const std::filesystem::path &path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        return normalized;
    }

    return path.lexically_normal();
}

inline bool is_path_within_workspace(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
    auto path_it = path.begin();
    auto root_it = workspace_root.begin();
    for (; root_it != workspace_root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) {
            return false;
        }
    }

    return true;
}

inline std::filesystem::path resolve_tool_path(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
    if (path.empty()) {
        throw std::runtime_error("path is empty");
    }
    if (workspace_root.empty()) {
        return path;
    }

    const auto normalized_workspace = normalize_tool_path(workspace_root);
    const auto candidate = path.is_absolute() ? path : normalized_workspace / path;
    const auto normalized_candidate = normalize_tool_path(candidate);
    if (!is_path_within_workspace(normalized_candidate, normalized_workspace)) {
        throw std::runtime_error("path escapes workspace sandbox: " + path.string());
    }

    return normalized_candidate;
}

inline std::filesystem::path resolve_tool_working_dir(const std::string &working_dir, const std::filesystem::path &workspace_root) {
    if (working_dir.empty()) {
        return workspace_root.empty() ? std::filesystem::path{} : normalize_tool_path(workspace_root);
    }

    auto resolved = resolve_tool_path(std::filesystem::path(working_dir), workspace_root);
    std::error_code ec;
    const bool exists = std::filesystem::exists(resolved, ec);
    if (ec) {
        throw std::runtime_error("failed to inspect working directory: " + resolved.string() + ": " + ec.message());
    }
    if (!exists) {
        throw std::runtime_error("working directory not found: " + resolved.string());
    }
    if (!std::filesystem::is_directory(resolved, ec) || ec) {
        throw std::runtime_error("working directory is not a directory: " + resolved.string());
    }

    return resolved;
}

void register_shell_tool(ToolRegistry &registry, const std::string &workspace);
void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);
void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);
void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);

} // namespace orangutan
