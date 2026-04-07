#pragma once

#include "permissions/permission-types.hpp"
#include "process/subprocess.hpp"
#include "tools/registry/tool-registry.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace orangutan::tools {

    class BackgroundCompletionDispatcher;


    using ContextualToolRegistrar = void (*)(ToolRegistry &, const ToolRuntimeContext *);

    template <std::size_t N>
    void register_contextual_tools(ToolRegistry &registry, const ToolRuntimeContext *tool_context, const std::array<ContextualToolRegistrar, N> &registrars) {
        if (tool_context == nullptr) {
            return;
        }

        for (const auto registrar : registrars) {
            registrar(registry, tool_context);
        }
    }
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

    inline std::filesystem::path normalize_permission_root(const std::filesystem::path &path, const std::filesystem::path &workspace_root) {
        if (path.empty()) {
            return {};
        }

        auto expanded = expand_tool_home_path(path);
        if (!expanded.is_absolute() && !workspace_root.empty()) {
            expanded = normalize_tool_path(workspace_root) / expanded;
        }
        return normalize_tool_path(expanded);
    }

    inline std::filesystem::path orangutan_config_root() {
        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            return {};
        }

        return normalize_tool_path(std::filesystem::path(home) / ".orangutan");
    }

    inline std::vector<std::filesystem::path> permission_roots(const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions) {
        std::vector<std::filesystem::path> roots;
        if (!workspace_root.empty()) {
            roots.push_back(normalize_tool_path(workspace_root));
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
        return std::ranges::any_of(permission_roots(workspace_root, permissions),
                                   [&path](const auto &root) {
                                       return is_path_within_workspace(path, root);
                                   });
    }

    inline std::filesystem::path resolve_tool_path(const std::filesystem::path &path, const std::filesystem::path &workspace_root,
                                                   const ToolPermissionContext *permissions = nullptr) {
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
        if (!is_tool_path_allowed(normalized_candidate, normalized_workspace, permissions)) {
            throw std::runtime_error("path escapes workspace sandbox: " + path.string());
        }

        return normalized_candidate;
    }

    inline std::filesystem::path resolve_tool_working_dir(std::string_view working_dir, const std::filesystem::path &workspace_root,
                                                          const ToolPermissionContext *permissions = nullptr) {
        if (working_dir.empty()) {
            return workspace_root.empty() ? std::filesystem::path{} : normalize_tool_path(workspace_root);
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

    void register_shell_tool(ToolRegistry &registry, const std::string &workspace, const ToolPermissionContext *permissions,
                             const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_process_tools(ToolRegistry &registry, const std::shared_ptr<BackgroundProcessManager> &process_manager);
    void register_read_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr,
                            std::string_view edit_mode = "search_replace");
    void register_write_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr);
    void register_edit_tool(ToolRegistry &registry, const std::filesystem::path &workspace_root, const ToolPermissionContext *permissions = nullptr,
                            std::string_view edit_mode = "search_replace");

} // namespace orangutan::tools
