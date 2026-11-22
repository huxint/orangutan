#pragma once

#include <cstdint>

namespace orangutan::hook {

enum class Event : std::uint8_t {
  channel_action,
  provider_request,
  provider_response,
  provider_error,
  tool_before,
  tool_after,
  memory_read_after,
  memory_write_before,
  memory_write_after,
  memory_forget,
  permission_ask_rendered,
};

/// Effect gates: events whose sinks decide before the effect runs. Every other
/// event is an advisory observation.
[[nodiscard]] constexpr bool is_gate(Event event) noexcept {
  switch (event) {
    case Event::tool_before:
    case Event::permission_ask_rendered:
    case Event::memory_write_before:
      return true;
    default:
      return false;
  }
}

}  // namespace orangutan::hook
