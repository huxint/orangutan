#pragma once

#include "tools/registry/permissions.hpp"
#include "tools/registry/tool.hpp"
#include "process/subprocess.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace orangutan {

    class BackgroundCompletionDispatcher;

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

    inline std::filesystem::path expand_tool_home_path(const std::filesystem::path &path) {
        const auto raw = path.string();
        if (raw != "~" && !raw.starts_with("~/")) {
            return path;
        }

        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            throw std::runtime_error("HOME is not set, cannot expand path: " + raw);
        }

        if (raw == "~") {
            return {home};
        }

        return std::filesystem::path(home) / raw.substr(2);
    }

    inline std::filesystem::path orangutan_config_root() {
        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            return {};
        }

        return normalize_tool_path(std::filesystem::path(home) / ".orangutan");
    }

    inline bool is_tool_path_allowed(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
        if (!workspace_root.empty() && is_path_within_workspace(path, workspace_root)) {
            return true;
        }

        const auto config_root = orangutan_config_root();
        return !config_root.empty() && is_path_within_workspace(path, config_root);
    }

    inline std::filesystem::path resolve_tool_path(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
        if (path.empty()) {
            throw std::runtime_error("path is empty");
        }

        auto expanded = expand_tool_home_path(path);
        if (workspace_root.empty()) {
            return expanded;
        }

        const auto normalized_workspace = normalize_tool_path(workspace_root);
        const auto candidate = expanded.is_absolute() ? expanded : normalized_workspace / expanded;
        const auto normalized_candidate = normalize_tool_path(candidate);
        if (!is_tool_path_allowed(normalized_candidate, normalized_workspace)) {
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

    void register_shell_tool(ToolRegistry &registry, const std::string &workspace, const ToolPermissionSettings *permissions,
                             const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_process_tools(ToolRegistry &registry, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, std::string_view edit_mode = "search_replace");
    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root);
    void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, std::string_view edit_mode = "search_replace");

} // namespace orangutan
