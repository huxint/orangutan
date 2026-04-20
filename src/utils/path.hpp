#pragma once

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
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
    inline std::expected<std::filesystem::path, std::string> try_expand_home_path(const std::filesystem::path &path) {
        const auto raw = path.string();
        if (raw != "~" && !std::string_view{raw}.starts_with("~/") && !std::string_view{raw}.starts_with("~\\")) {
            return path;
        }

        const auto *home = std::getenv("HOME");
        if (home == nullptr || std::string_view{home}.empty()) {
            return std::unexpected("HOME is not set, cannot expand path: " + raw);
        }

        if (raw == "~") {
            return std::filesystem::path{home};
        }

        return std::filesystem::path{home} / raw.substr(2);
    }

    [[nodiscard]]
    inline std::filesystem::path expand_home_path(const std::filesystem::path &path) {
        auto expanded = try_expand_home_path(path);
        if (expanded.has_value()) {
            return *expanded;
        }
        throw std::runtime_error(expanded.error());
    }

    [[nodiscard]]
    inline std::optional<std::filesystem::path> find_in_path(std::string_view binary) {
        const auto *path_env = std::getenv("PATH");
        if (path_env == nullptr) {
            return std::nullopt;
        }

        constexpr auto EXEC_BITS = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;

        for (auto entry : std::string_view{path_env} | std::views::split(':')) {
            const std::string_view dir{entry};
            if (dir.empty()) {
                continue;
            }

            auto candidate = std::filesystem::path{dir} / binary;
            std::error_code ec;
            const auto status = std::filesystem::status(candidate, ec);
            if (!ec && status.type() != std::filesystem::file_type::not_found && (status.permissions() & EXEC_BITS) != std::filesystem::perms::none) {
                return candidate;
            }
        }

        return std::nullopt;
    }

    [[nodiscard]]
    inline bool path_has_prefix(const std::filesystem::path &path, const std::filesystem::path &root) {
        const auto normalized_root = normalize_path(root);
        if (normalized_root.empty()) {
            return false;
        }
        const auto normalized_path = normalize_path(path);
        const auto mismatch = std::ranges::mismatch(normalized_root, normalized_path);
        return mismatch.in1 == normalized_root.end();
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
    using utils::find_in_path;
    using utils::normalize_path;
    using utils::path_has_prefix;
    using utils::resolve_relative_to;
    using utils::try_expand_home_path;

} // namespace orangutan
