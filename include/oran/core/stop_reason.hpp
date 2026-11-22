// include/oran/core/stop_reason.hpp — terminal cause of a model turn.
//
// Stable string spelling comes from `core::enum_name(reason)` /
// `core::parse_enum<StopReason>(text)` in `enum_names.hpp`. The
// `std::formatter` specialization keeps `std::print("{}", reason)` ergonomic.

#pragma once

#include <cstdint>
#include <format>
#include <string_view>

#include <oran/core/enum_names.hpp>

namespace orangutan::core {

enum class StopReason : std::uint8_t {
  end_turn,       ///< Model decided it was done.
  max_tokens,     ///< Token budget exhausted.
  tool_use,       ///< Model wants a tool call before continuing.
  stop_sequence,  ///< Configured stop string matched.
  cancelled,      ///< Execution layer (or user) cancelled.
  error,          ///< Protocol/upstream failure surfaced as a stop reason.
};

}  // namespace orangutan::core

template <>
struct std::formatter<orangutan::core::StopReason> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::core::StopReason s, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(s), ctx);
  }
};
