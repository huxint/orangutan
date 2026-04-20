#pragma once

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
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
        const auto to_string_view = [](auto &&part) -> std::string_view {
            const auto first = std::ranges::begin(part);
            const auto last = std::ranges::end(part);
            if (first == last) {
                return {};
            }
            return {std::to_address(first), static_cast<std::size_t>(std::ranges::distance(part))};
        };

        for (auto &&entry : std::string_view{path_env} | std::views::split(':')) {
            const auto dir = to_string_view(entry);
            if (dir.empty()) {
                continue;
            }

            const auto candidate = std::filesystem::path{dir} / binary;
            std::error_code ec;
            const auto status = std::filesystem::status(candidate, ec);
            if (!ec && std::filesystem::exists(status) && (status.permissions() & EXEC_BITS) != std::filesystem::perms::none) {
                return candidate;
            }
        }

        return std::nullopt;
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
    using utils::find_in_path;
    using utils::normalize_path;
    using utils::path_has_prefix;
    using utils::resolve_relative_to;
    using utils::try_expand_home_path;

} // namespace orangutan
