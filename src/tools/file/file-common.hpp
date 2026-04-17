#pragma once

#include "permissions/permission-types.hpp"
#include "tools/internal.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace orangutan::tools::file {

    /// Validate that a single required string path field in `input` points inside the sandbox.
    inline PermissionResult validate_required_path(const nlohmann::json &input, std::string_view key, const std::filesystem::path &workspace_root,
                                                   const ToolPermissionContext &ctx) {
        const std::string key_str{key};
        if (!input.contains(key_str) || !input.at(key_str).is_string()) {
            return PermissionResult::deny(key_str + " path is required");
        }
        return validate_path_permission(input.at(key_str).get<std::string>(), workspace_root, ctx);
    }

    /// Validate an optional string path field — passthrough when absent.
    inline PermissionResult validate_optional_path(const nlohmann::json &input, std::string_view key, const std::filesystem::path &workspace_root,
                                                   const ToolPermissionContext &ctx) {
        const std::string key_str{key};
        if (!input.contains(key_str) || !input.at(key_str).is_string()) {
            return PermissionResult::passthrough();
        }
        return validate_path_permission(input.at(key_str).get<std::string>(), workspace_root, ctx);
    }

    /// Validate an optional array-of-string paths field — denies if non-string entries are present.
    inline PermissionResult validate_optional_paths(const nlohmann::json &input, std::string_view key, const std::filesystem::path &workspace_root,
                                                    const ToolPermissionContext &ctx) {
        const std::string key_str{key};
        if (!input.contains(key_str) || !input.at(key_str).is_array()) {
            return PermissionResult::passthrough();
        }
        for (const auto &item : input.at(key_str)) {
            if (!item.is_string()) {
                return PermissionResult::deny(key_str + " entries must be strings");
            }
            if (auto result = validate_path_permission(item.get<std::string>(), workspace_root, ctx); !result.is_passthrough) {
                return result;
            }
        }
        return PermissionResult::passthrough();
    }

    /// Resolve a string path under `key` inside the sandbox. Caller asserts the field exists.
    inline std::filesystem::path resolve_path_field(const nlohmann::json &input, std::string_view key, const std::filesystem::path &workspace_root,
                                                    const ToolPermissionContext *permissions) {
        return resolve_tool_path(std::filesystem::path(input.at(std::string{key}).get<std::string>()), workspace_root, permissions);
    }

} // namespace orangutan::tools::file
