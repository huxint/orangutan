#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace orangutan::utils {

    template <typename associative_container>
    concept has_transparent_unordered_lookup = requires {
        typename associative_container::hasher::is_transparent;
        typename associative_container::key_equal::is_transparent;
    };

    template <typename associative_container>
    concept has_transparent_ordered_lookup = requires { typename associative_container::key_compare::is_transparent; };

    template <typename associative_container>
    constexpr bool HAS_TRANSPARENT_LOOKUP = has_transparent_unordered_lookup<associative_container> || has_transparent_ordered_lookup<associative_container>;

    struct TransparentStringHash {
        using is_transparent = void;

        [[nodiscard]]
        std::size_t operator()(std::string_view value) const noexcept {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct TransparentStringEqual {
        using is_transparent = void;

        [[nodiscard]]
        bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
            return lhs == rhs;
        }
    };

    template <typename value_type>
    using transparent_string_unordered_map = std::unordered_map<std::string, value_type, TransparentStringHash, TransparentStringEqual>;

    template <typename associative_container>
    [[nodiscard]]
    auto transparent_find(associative_container &container, std::string_view key) {
        if constexpr (HAS_TRANSPARENT_LOOKUP<associative_container>) {
            return container.find(key);
        } else {
            return container.find(std::string{key});
        }
    }

    template <typename associative_container>
    [[nodiscard]]
    auto transparent_find(const associative_container &container, std::string_view key) {
        if constexpr (HAS_TRANSPARENT_LOOKUP<associative_container>) {
            return container.find(key);
        } else {
            return container.find(std::string{key});
        }
    }

    template <typename associative_container>
    [[nodiscard]]
    bool transparent_contains(const associative_container &container, std::string_view key) {
        if constexpr (HAS_TRANSPARENT_LOOKUP<associative_container>) {
            if constexpr (requires { container.contains(key); }) {
                return container.contains(key);
            }
            return container.find(key) != container.end();
        } else {
            return container.find(std::string{key}) != container.end();
        }
    }

} // namespace orangutan::utils
