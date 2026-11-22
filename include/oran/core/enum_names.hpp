// include/oran/core/enum_names.hpp — reflection-backed enum<->string helpers.
//
// One canonical pair of helpers replaces hand-maintained string tables for
// every `enum class` in the repo: `enum_name(value)` returns the identifier
// (or `"unknown"` for an out-of-range cast); `parse_enum<E>(text)` is the
// inverse, returning `std::nullopt` when `text` is not a valid spelling.
// `enum_values<E>()` exposes every enumerator in declaration order. Callers
// reach for these directly — no per-enum wrapper functions.
//
// **Heavy include.** This pulls in `<meta>`. Including it from a public enum
// header is acceptable because the header is the natural place to mention
// the formatter and the helper; including it from an unrelated public
// header is not.
//
// Wire-name convention: a trailing underscore on an enumerator name (used to
// dodge a C++ keyword, e.g. `Mode::default_`) is stripped from the wire
// spelling. Any other identifier-style spelling is preserved verbatim.
// Enumerators whose wire format deviates from the identifier (dashes,
// alternate casing, …) cannot use this helper and must stay hand-written.

#pragma once

#include <array>
#include <cstddef>
#include <meta>
#include <optional>
#include <string_view>

namespace orangutan::core {

namespace meta_detail {

[[nodiscard]] constexpr std::string_view strip_keyword_suffix(std::string_view name) noexcept {
  return (!name.empty() && name.back() == '_') ? name.substr(0, name.size() - 1) : name;
}

}  // namespace meta_detail

template <typename E>
constexpr std::size_t enum_count_v = std::meta::enumerators_of(^^E).size();

/// Stable identifier-style spelling for `value`, or `"unknown"` when `value`
/// is an out-of-range cast. Trailing-underscore identifiers are stripped to
/// recover the natural wire spelling (see file header).
template <typename E>
[[nodiscard]] constexpr std::string_view enum_name(E value) noexcept {
  std::string_view result = "unknown";
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (value == [:e:]) {
      result = meta_detail::strip_keyword_suffix(std::meta::identifier_of(e));
    }
  }
  return result;
}

/// Inverse of `enum_name`. Returns `std::nullopt` when `text` does not match
/// any enumerator's wire spelling, including the `"unknown"` fallback.
template <typename E>
[[nodiscard]] constexpr std::optional<E> parse_enum(std::string_view text) noexcept {
  std::optional<E> result;
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    if (text == meta_detail::strip_keyword_suffix(std::meta::identifier_of(e))) {
      result = [:e:];
    }
  }
  return result;
}

/// Read-only view over every enumerator in declaration order.
template <typename E>
[[nodiscard]] constexpr auto enum_values() noexcept {
  std::array<E, enum_count_v<E>> result{};
  std::size_t i = 0;
  template for (constexpr auto e : std::define_static_array(std::meta::enumerators_of(^^E))) {
    result[i++] = [:e:];
  }
  return result;
}

}  // namespace orangutan::core
