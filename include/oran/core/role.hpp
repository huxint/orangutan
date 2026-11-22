// include/oran/core/role.hpp — message author identity.
//
// `Role` is the author of a `core::Message`. Stable string spelling lives in
// the type itself: callers use `core::enum_name(role)` /
// `core::parse_enum<Role>(text)` from `enum_names.hpp` for the wire format.
// The `std::formatter` specialization below keeps `std::print("{}", role)`
// ergonomic.

#pragma once

#include <cstdint>
#include <format>
#include <string_view>

#include <oran/core/enum_names.hpp>

namespace orangutan::core {

enum class Role : std::uint8_t {
  user,
  assistant,
  system,
  tool,
};

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::Role> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::Role r, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(r), ctx);
  }
};
