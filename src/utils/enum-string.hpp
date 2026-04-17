#pragma once

#include "utils/string.hpp"

#include <array>
#include <cstddef>
#include <magic_enum/magic_enum.hpp>
#include <optional>
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

    namespace detail {

        /// Per-enumerator static buffer holding the dash-separated spelling.
        /// `magic_enum::enum_name<V>()` is constexpr, so the transformation
        /// happens at compile time and the buffer has static storage duration —
        /// safe to return as `std::string_view`.
        template <auto V>
            requires std::is_enum_v<decltype(V)>
        inline constexpr auto KEBAB_STORAGE = [] {
            constexpr auto NAME = magic_enum::enum_name<V>();
            std::array<char, NAME.size()> buffer{};
            for (std::size_t i = 0; i < NAME.size(); ++i) {
                buffer[i] = NAME[i] == '_' ? '-' : NAME[i];
            }
            return buffer;
        }();

    } // namespace detail

    /// Like `enum_name`, but replaces underscores with dashes — useful for
    /// protocol tokens such as `chat_completions` → `chat-completions`.
    /// The result points into a per-enumerator static buffer so it can be
    /// returned as a `std::string_view` without allocation.
    template <typename Enum>
        requires std::is_enum_v<Enum>
    [[nodiscard]]
    constexpr std::string_view enum_name_kebab(Enum value) noexcept {
        std::string_view result;
        constexpr auto VALUES = magic_enum::enum_values<Enum>();
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((value == VALUES[I] ? (result = std::string_view(detail::KEBAB_STORAGE<VALUES[I]>.data(),
                                                              detail::KEBAB_STORAGE<VALUES[I]>.size()),
                                    0)
                                 : 0),
             ...);
        }(std::make_index_sequence<VALUES.size()>{});
        return result;
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
