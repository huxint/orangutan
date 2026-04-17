#pragma once

#include "permissions/permission-types.hpp"
#include "process/subprocess.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/path.hpp"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orangutan::tools {

    class BackgroundCompletionDispatcher;

    inline std::filesystem::path normalize_permission_root(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
        if (path.empty()) {
            return {};
        }

        auto expanded = utils::expand_home_path(path);
        if (!expanded.is_absolute() && !workspace_root.empty()) {
            expanded = utils::resolve_relative_to(expanded, workspace_root);
        }
        return utils::normalize_path(expanded);
    }

    inline std::filesystem::path orangutan_config_root() {
        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            return {};
        }

        return utils::normalize_path(std::filesystem::path(home) / ".orangutan");
    }

    inline std::vector<std::filesystem::path> permission_roots(const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        std::vector<std::filesystem::path> roots;
        if (!workspace_root.empty()) {
            roots.push_back(utils::normalize_path(workspace_root));
        }

        const auto config_root = orangutan_config_root();
        if (!config_root.empty()) {
            roots.push_back(config_root);
        }

        if (permissions != nullptr) {
            for (const auto &dir : permissions->additional_directories) {
                auto root = normalize_permission_root(std::filesystem::path(dir), workspace_root);
                if (!root.empty()) {
                    roots.push_back(std::move(root));
                }
            }
        }

        return roots;
    }

    inline bool is_tool_path_allowed(const std::filesystem::path &path, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr) {
        return std::ranges::any_of(permission_roots(workspace_root, permissions), [&path](const auto &root) {
            return utils::path_has_prefix(path, root);
        });
    }

    inline std::filesystem::path resolve_tool_path(const std::filesystem::path &path, const std::filesystem::path &workspace_root,
                                                   const ToolPermissionContext *permissions = nullptr) {
        if (path.empty()) {
            throw std::runtime_error("path is empty");
        }

        auto expanded = utils::expand_home_path(path);
        if (workspace_root.empty()) {
            return expanded;
        }

        const auto normalized_workspace = utils::normalize_path(workspace_root);
        const auto normalized_candidate = expanded.is_absolute() ? utils::normalize_path(expanded) : utils::resolve_relative_to(expanded, normalized_workspace);
        if (!is_tool_path_allowed(normalized_candidate, normalized_workspace, permissions)) {
            throw std::runtime_error("path escapes workspace sandbox: " + path.string());
        }

        return normalized_candidate;
    }

    inline std::filesystem::path resolve_tool_working_dir(std::string_view working_dir, const std::filesystem::path &workspace_root,
                                                          const ToolPermissionContext *permissions = nullptr) {
        if (working_dir.empty()) {
            return workspace_root.empty() ? std::filesystem::path{} : utils::normalize_path(workspace_root);
        }

        auto resolved = resolve_tool_path(std::filesystem::path(working_dir), workspace_root, permissions);
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

    /// Run resolve_tool_path and fold any exception into a PermissionResult::deny.
    /// Returns passthrough() when the path is within the permitted sandbox.
    inline PermissionResult validate_path_permission(std::string_view path, const std::filesystem::path &workspace_root, const ToolPermissionContext &ctx) {
        try {
            static_cast<void>(resolve_tool_path(std::filesystem::path(path), workspace_root, &ctx));
        } catch (const std::exception &e) {
            return PermissionResult::deny(e.what());
        }
        return PermissionResult::passthrough();
    }

    void register_shell_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions,
                             const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_process_tools(ToolRegistry &registry, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr,
                            std::string_view edit_mode = "search_replace");
    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr);
    void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr,
                            std::string_view edit_mode = "search_replace");
    void register_fs_tools(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr);

} // namespace orangutan::tools
