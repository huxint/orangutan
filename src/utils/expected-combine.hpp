#pragma once

#include <expected>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace orangutan::utils {

    namespace detail {

        template <typename T>
        using all_ok_value_t = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    } // namespace detail

    /// Combine multiple `std::expected` values sharing the same error type into
    /// a single `std::expected<tuple, E>`. Short-circuits on the first error
    /// (scanning left-to-right) and preserves argument positions in the tuple
    /// so structured bindings line up with call order — `std::expected<void, E>`
    /// slots are kept as `std::monostate` placeholders so you can ignore them
    /// with `_`:
    ///
    ///     auto parts = utils::all_ok(
    ///         validate(value),                       // expected<void, E>
    ///         parse_required_string_field(value, k), // expected<string, E>
    ///         parse_time_zone_field(value));         // expected<string, E>
    ///     if (!parts.has_value()) {
    ///         return std::unexpected(parts.error());
    ///     }
    ///     auto &[_, key, tz] = *parts;
    ///
    /// All arguments are eagerly evaluated — fine for pure validators/parsers.
    /// For side-effecting or expensive steps that must short-circuit, chain
    /// with `std::expected::and_then` instead.
    template <typename E, typename... Ts>
    [[nodiscard]]
    std::expected<std::tuple<detail::all_ok_value_t<Ts>...>, E> all_ok(std::expected<Ts, E>... results) {
        if constexpr (sizeof...(Ts) == 0) {
            return std::expected<std::tuple<>, E>{};
        } else {
            std::optional<E> first_error;
            auto scan = [&](auto &result) {
                if (!first_error.has_value() && !result.has_value()) {
                    first_error = std::move(result).error();
                }
            };
            (scan(results), ...);

            if (first_error.has_value()) {
                return std::unexpected(std::move(*first_error));
            }

            auto extract = []<typename R>(R &&result) {
                using value_type = typename std::remove_cvref_t<R>::value_type;
                if constexpr (std::is_void_v<value_type>) {
                    return std::monostate{};
                } else {
                    return *std::forward<R>(result);
                }
            };

            return std::tuple<detail::all_ok_value_t<Ts>...>(extract(std::move(results))...);
        }
    }

} // namespace orangutan::utils
