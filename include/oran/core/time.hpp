// include/oran/core/time.hpp — `Time` value type and ISO 8601 UTC helpers.
//
// `core::Time` is the canonical absolute-time type that crosses library
// boundaries. The wire format used by storage rows, audit logs, and memory
// records is `YYYY-MM-DDTHH:MM:SS[.fff]Z` — strict UTC, millisecond precision
// optional but capped. The helpers below own that contract.

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::core {

/// Absolute UTC time point. Strong wrapper around
/// `std::chrono::system_clock::time_point` so that calling code cannot mix the
/// raw chrono type (which has implementation-defined epoch and resolution)
/// with values that are supposed to round-trip through the documented wire
/// format.
class Time {
public:
  using clock = std::chrono::system_clock;
  using time_point = clock::time_point;

  constexpr Time() noexcept = default;
  constexpr explicit Time(time_point tp) noexcept : tp_{tp} {}

  [[nodiscard]] constexpr time_point to_system_time_point() const noexcept {
    return tp_;
  }

  /// `Time` at the UNIX epoch (1970-01-01T00:00:00.000Z). Useful for tests
  /// and as the documented zero value.
  [[nodiscard]] static constexpr Time epoch() noexcept {
    return Time{time_point{}};
  }

  friend constexpr auto operator<=>(Time, Time) noexcept = default;

private:
  time_point tp_{};
};

namespace time {

/// Current absolute UTC time read from `std::chrono::system_clock`. `noexcept`
/// because the standard library guarantees a non-throwing `now()` on the
/// clock.
[[nodiscard]] Time now_utc() noexcept;

/// Render a `Time` in the canonical wire format. Always emits exactly
/// `YYYY-MM-DDTHH:MM:SS.fffZ` — fixed-width year (>= 4 digits), millisecond
/// precision, trailing `Z`. Years are clamped to the four-digit range
/// [0000, 9999]; out-of-range inputs return that clamp's nearest value rather
/// than failing, so call sites that need a stricter check should validate
/// before formatting.
[[nodiscard]] std::string format_iso8601_utc(Time);

/// Strict parser for the canonical wire format. Accepts:
///
/// - `YYYY-MM-DDTHH:MM:SSZ`
/// - `YYYY-MM-DDTHH:MM:SS.fZ`, `.ffZ`, or `.fffZ`
///
/// Rejects empty input (`ErrorKind::invalid_argument`), unknown shape
/// (`ErrorKind::parsing`), more than three fractional digits, missing `Z`,
/// non-UTC offsets, lowercase separators, leading/trailing whitespace, and
/// any out-of-range field.
[[nodiscard]] Result<Time> parse_iso8601_utc(std::string_view input);

}  // namespace time

}  // namespace orangutan::core
