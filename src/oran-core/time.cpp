// src/oran-core/time.cpp — `core::Time` + ISO 8601 UTC helpers.

#include <oran/core/time.hpp>

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::core::time {

Time now_utc() noexcept {
  return Time{std::chrono::system_clock::now()};
}

namespace {

constexpr bool all_digits(std::string_view s) noexcept {
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

constexpr int parse_fixed_width(std::string_view s) noexcept {
  int value = 0;
  for (const char c : s) {
    value = value * 10 + (c - '0');
  }
  return value;
}

[[nodiscard]] Error parse_error(std::string message, std::string_view input) {
  return Error::parsing(std::move(message)).with("input", std::string{input});
}

}  // namespace

std::string format_iso8601_utc(Time t) {
  using namespace std::chrono;

  const auto tp_ms = floor<milliseconds>(t.to_system_time_point());
  const auto day_point = floor<days>(tp_ms);
  const year_month_day ymd{day_point};
  const hh_mm_ss<milliseconds> tod{tp_ms - day_point};

  int year = static_cast<int>(ymd.year());
  if (year < 0) {
    year = 0;
  } else if (year > 9999) {
    year = 9999;
  }

  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                     year,
                     static_cast<unsigned>(ymd.month()),
                     static_cast<unsigned>(ymd.day()),
                     static_cast<int>(tod.hours().count()),
                     static_cast<int>(tod.minutes().count()),
                     static_cast<int>(tod.seconds().count()),
                     static_cast<int>(tod.subseconds().count()));
}

Result<Time> parse_iso8601_utc(std::string_view input) {
  using namespace std::chrono;

  if (input.empty()) {
    return std::unexpected(Error::invalid_argument("empty iso8601 timestamp"));
  }

  // Accepted shapes by length:
  //   20: YYYY-MM-DDTHH:MM:SSZ
  //   22: YYYY-MM-DDTHH:MM:SS.fZ
  //   23: YYYY-MM-DDTHH:MM:SS.ffZ
  //   24: YYYY-MM-DDTHH:MM:SS.fffZ
  const auto n = input.size();
  if (n != 20 && n != 22 && n != 23 && n != 24) {
    return std::unexpected(parse_error("iso8601: unexpected length", input).with("length", std::to_string(n)));
  }

  if (input[4] != '-' || input[7] != '-' || input[10] != 'T' || input[13] != ':' || input[16] != ':' ||
      input.back() != 'Z') {
    return std::unexpected(parse_error("iso8601: bad separators", input));
  }

  const std::string_view year_str = input.substr(0, 4);
  const std::string_view month_str = input.substr(5, 2);
  const std::string_view day_str = input.substr(8, 2);
  const std::string_view hour_str = input.substr(11, 2);
  const std::string_view minute_str = input.substr(14, 2);
  const std::string_view second_str = input.substr(17, 2);

  if (!all_digits(year_str) || !all_digits(month_str) || !all_digits(day_str) || !all_digits(hour_str) ||
      !all_digits(minute_str) || !all_digits(second_str)) {
    return std::unexpected(parse_error("iso8601: non-digit field", input));
  }

  int ms_value = 0;
  if (n > 20) {
    if (input[19] != '.') {
      return std::unexpected(parse_error("iso8601: missing fractional separator", input));
    }
    const std::size_t frac_len = n - 21;  // exclude leading '.' and trailing 'Z'
    const std::string_view frac = input.substr(20, frac_len);
    if (!all_digits(frac)) {
      return std::unexpected(parse_error("iso8601: non-digit fraction", input));
    }
    const int frac_value = parse_fixed_width(frac);
    constexpr std::array<int, 4> kScales{0, 100, 10, 1};
    ms_value = frac_value * kScales[frac_len];
  }

  const int year_value = parse_fixed_width(year_str);
  const int month_value = parse_fixed_width(month_str);
  const int day_value = parse_fixed_width(day_str);
  const int hour_value = parse_fixed_width(hour_str);
  const int minute_value = parse_fixed_width(minute_str);
  const int second_value = parse_fixed_width(second_str);

  if (month_value < 1 || month_value > 12) {
    return std::unexpected(
        parse_error("iso8601: month out of range", input).with("month", std::to_string(month_value)));
  }
  if (day_value < 1 || day_value > 31) {
    return std::unexpected(parse_error("iso8601: day out of range", input).with("day", std::to_string(day_value)));
  }
  if (hour_value > 23) {
    return std::unexpected(parse_error("iso8601: hour out of range", input).with("hour", std::to_string(hour_value)));
  }
  if (minute_value > 59) {
    return std::unexpected(
        parse_error("iso8601: minute out of range", input).with("minute", std::to_string(minute_value)));
  }
  if (second_value > 59) {
    return std::unexpected(
        parse_error("iso8601: second out of range", input).with("second", std::to_string(second_value)));
  }

  const year_month_day ymd{year{year_value} / month{static_cast<unsigned>(month_value)} /
                           day{static_cast<unsigned>(day_value)}};
  if (!ymd.ok()) {
    return std::unexpected(parse_error("iso8601: invalid calendar date", input));
  }

  const auto day_point = sys_days{ymd};
  const auto tp =
      day_point + hours{hour_value} + minutes{minute_value} + seconds{second_value} + milliseconds{ms_value};
  return Time{tp};
}

}  // namespace orangutan::core::time
