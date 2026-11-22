#pragma once

#include <expected>
#include <type_traits>

#include <oran/core/error.hpp>

namespace orangutan::core {

template <typename T>
using Result = std::expected<T, Error>;

namespace detail {

template <typename T>
struct is_result : std::false_type {};
template <typename T>
struct is_result<Result<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_result_v = is_result<std::remove_cvref_t<T>>::value;

}  // namespace detail

}  // namespace orangutan::core
