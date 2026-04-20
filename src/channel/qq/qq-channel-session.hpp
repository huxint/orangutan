#pragma once

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    [[nodiscard]]
    inline std::string getenv_or_default(const char *name, const char *fallback) {
        const char *value = std::getenv(name);
        if (value == nullptr || *value == '\0') {
            return fallback;
        }
        return value;
    }

    [[nodiscard]]
    inline std::string sanitize_path_component(std::string input) {
        for (auto &ch : input) {
            if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '-' && ch != '_') {
                ch = '_';
            }
        }
        if (input.empty()) {
            return "default";
        }
        return input;
    }

    [[nodiscard]]
    inline std::filesystem::path qq_session_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "sessions" / ("session-" + safe_name + ".json");
    }

    [[nodiscard]]
    inline std::filesystem::path qq_known_users_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "known-users" / ("known-users-" + safe_name + ".json");
    }

    [[nodiscard]]
    inline std::filesystem::path qq_ref_index_file_path(std::string_view bot_name) {
        const auto home = getenv_or_default("HOME", ".");
        const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
        return std::filesystem::path(home) / ".orangutan" / "qq" / "ref-index" / ("ref-index-" + safe_name + ".jsonl");
    }

} // namespace orangutan::channel::qq
