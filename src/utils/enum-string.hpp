#pragma once

#include "utils/string.hpp"

#include <algorithm>
#include <magic_enum/magic_enum.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace orangutan::utils {

    /// Thin, greppable wrapper around magic_enum::enum_name. Prefer this over
    /// hand-rolled switch-based `to_string` helpers for internal enums whose
    /// identifier already matches the serialised form.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]]
    constexpr std::string_view enum_name(Enum value) noexcept {
        return magic_enum::enum_name(value);
    }

    /// Like `enum_name`, but replaces underscores with dashes — useful for
    /// protocol tokens such as `chat_completions` → `chat-completions`.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]]
    inline std::string enum_name_kebab(Enum value) {
        std::string out{magic_enum::enum_name(value)};
        std::ranges::replace(out, '_', '-');
        return out;
    }

    /// Parse a token into Enum via magic_enum, tolerating case, dashes, and
    /// underscores. Returns std::nullopt on unknown values.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]]
    inline std::optional<Enum> parse_enum(std::string_view token) noexcept {
        return magic_enum::enum_cast<Enum>(normalize_enum_token(token), magic_enum::case_insensitive);
    }

    /// Same as parse_enum, but returns `fallback` on unknown or empty input.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]]
    inline Enum parse_enum_or(std::string_view token, Enum fallback) noexcept {
        if (token.empty()) {
            return fallback;
        }
        return parse_enum<Enum>(token).value_or(fallback);
    }

} // namespace orangutan::utils
