#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::core {

/// Trace/audit correlation id for one agent turn.
///
/// The storage boundary persists this as a 16-byte BLOB. Core owns the shared
/// value shape plus its identity operations — zero detection, the canonical
/// operator-visible spelling, and generation — so agent, tool, permission,
/// hook, storage, and bootstrap code do not invent incompatible ids.
using TurnId = std::array<std::byte, 16>;

[[nodiscard]] inline bool is_zero_turn_id(const TurnId& id) noexcept {
  return std::ranges::all_of(id, [](std::byte byte) { return byte == std::byte{}; });
}

/// Canonical operator-visible spelling: 32 lowercase hex characters, the same
/// byte order the storage boundary's BLOB round-trip produces. `--trace`,
/// `--trace-export`, session-store keys, and trace JSON all use this spelling;
/// parsers accepting it must reject any other shape.
[[nodiscard]] std::string format_turn_id_hex(const TurnId& id);
[[nodiscard]] Result<TurnId> parse_turn_id_hex(std::string_view text);

/// Generate a fresh non-zero id: UUIDv4-shaped (version/variant bits set)
/// over `std::random_device` entropy mixed with a monotonic counter and the
/// current epoch-nanosecond timestamp. Fails only when the platform entropy
/// source throws.
[[nodiscard]] Result<TurnId> generate_turn_id();

}  // namespace orangutan::core
