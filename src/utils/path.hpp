#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace orangutan::utils {

    [[nodiscard]]
    inline std::filesystem::path normalize_path(const std::filesystem::path &path) {
        std::error_code ec;
        auto normalized = std::filesystem::weakly_canonical(path, ec);
        if (!ec) {
            return normalized;
        }

        return path.lexically_normal();
    }

    [[nodiscard]]
    inline std::filesystem::path expand_home_path(const std::filesystem::path &path) {
        const auto raw = path.string();
        if (raw != "~" && !std::string_view{raw}.starts_with("~/") && !std::string_view{raw}.starts_with("~\\")) {
            return path;
        }

        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            throw std::runtime_error("HOME is not set, cannot expand path: " + raw);
        }

        if (raw == "~") {
            return std::filesystem::path{home};
        }

        return std::filesystem::path{home} / raw.substr(2);
    }

    [[nodiscard]]
    inline bool path_has_prefix(const std::filesystem::path &path, const std::filesystem::path &root) {
        auto normalized_path = normalize_path(path);
        auto normalized_root = normalize_path(root);
        if (normalized_root.empty()) {
            return false;
        }

        auto path_it = normalized_path.begin();
        auto root_it = normalized_root.begin();
        for (; root_it != normalized_root.end(); ++root_it, ++path_it) {
            if (path_it == normalized_path.end() || *path_it != *root_it) {
                return false;
            }
        }

        return true;
    }

    [[nodiscard]]
    inline std::filesystem::path resolve_relative_to(const std::filesystem::path &path, const std::filesystem::path &root) {
        if (path.is_absolute()) {
            return normalize_path(path);
        }

        if (root.empty()) {
            return normalize_path(path);
        }

        return normalize_path(root / path);
    }

} // namespace orangutan::utils

namespace orangutan {

    using utils::expand_home_path;
    using utils::normalize_path;
    using utils::path_has_prefix;
    using utils::resolve_relative_to;

} // namespace orangutan
