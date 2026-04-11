#include "permissions/safety-checks.hpp"

#include "types/content.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace orangutan::permissions {

    static constexpr std::array PROTECTED_DIRS = {
        std::string_view{".git"},
        std::string_view{".orangutan"},
        std::string_view{".claude"},
    };

    static constexpr std::array PROTECTED_SUFFIXES = {
        std::string_view{".bashrc"},
        std::string_view{".zshrc"},
        std::string_view{".profile"},
        std::string_view{".bash_profile"},
    };

    bool is_protected_path(std::string_view path) {
        for (auto dir : PROTECTED_DIRS) {
            if (path == dir) {
                return true;
            }
            auto with_slash = std::string(dir) + "/";
            if (path.starts_with(with_slash)) {
                return true;
            }
            auto mid = std::string("/") + std::string(dir) + "/";
            if (path.contains(mid)) {
                return true;
            }
        }

        return std::ranges::any_of(PROTECTED_SUFFIXES, [&](auto suffix) {
            return path.ends_with(suffix);
        });
    }

    bool is_write_operation(const ToolUse &call) {
        if (call.name == "write" || call.name == "edit" || call.name == "file_write" || call.name == "file_edit") {
            return true;
        }
        if (call.name == "shell") {
            return true;
        }
        return false;
    }

    static std::string extract_path_for_safety(const ToolUse &call) {
        if (call.name == "read" || call.name == "write" || call.name == "edit" || call.name == "file_write" || call.name == "file_edit" || call.name == "file_read") {
            if (call.input.contains("path") && call.input["path"].is_string()) {
                return call.input["path"].get<std::string>();
            }
            if (call.input.contains("file_path") && call.input["file_path"].is_string()) {
                return call.input["file_path"].get<std::string>();
            }
        }
        if (call.name == "shell") {
            if (call.input.contains("command") && call.input["command"].is_string()) {
                return call.input["command"].get<std::string>();
            }
        }
        return {};
    }

    std::optional<PermissionDecision> check_safety(const ToolUse &call) {
        auto path = extract_path_for_safety(call);
        if (path.empty()) {
            return std::nullopt;
        }

        if (is_protected_path(path) && is_write_operation(call)) {
            return PermissionDecision::ask_by_safety(std::string(path), "Protected path: " + path);
        }

        return std::nullopt;
    }

} // namespace orangutan::permissions
