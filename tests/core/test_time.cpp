// tests/core/test_time.cpp — `core::Time` and ISO 8601 helpers coverage.

#include <chrono>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>

using orangutan::core::ErrorKind;
using orangutan::core::Time;
namespace ct = orangutan::core::time;

TEST_CASE("Time::epoch round-trips through the canonical wire format", "[unit][core][time]") {
  const auto rendered = ct::format_iso8601_utc(Time::epoch());
  REQUIRE(rendered == "1970-01-01T00:00:00.000Z");

  const auto parsed = ct::parse_iso8601_utc(rendered);
  REQUIRE(parsed.has_value());
  REQUIRE(*parsed == Time::epoch());
}

TEST_CASE("format_iso8601_utc emits fixed-width fields with millisecond precision", "[unit][core][time]") {
  using namespace std::chrono;
  const sys_days base{year{2026} / January / 7};
  const auto tp = base + hours{3} + minutes{4} + seconds{5} + milliseconds{6};

  const auto rendered = ct::format_iso8601_utc(Time{tp});
  REQUIRE(rendered == "2026-01-07T03:04:05.006Z");
}

TEST_CASE("format_iso8601_utc preserves ordering and round-trips for non-trivial dates", "[unit][core][time]") {
  using namespace std::chrono;
  const auto a_tp = sys_days{year{2024} / June / 30} + hours{12};
  const auto b_tp = sys_days{year{2025} / June / 30} + hours{12};
  const Time a{a_tp};
  const Time b{b_tp};

  REQUIRE(a < b);
  REQUIRE_FALSE(a == b);

  const auto a_str = ct::format_iso8601_utc(a);
  const auto b_str = ct::format_iso8601_utc(b);
  REQUIRE(a_str == "2024-06-30T12:00:00.000Z");
  REQUIRE(b_str == "2025-06-30T12:00:00.000Z");

  const auto a_parsed = ct::parse_iso8601_utc(a_str);
  const auto b_parsed = ct::parse_iso8601_utc(b_str);
  REQUIRE(a_parsed.has_value());
  REQUIRE(b_parsed.has_value());
  REQUIRE(*a_parsed == a);
  REQUIRE(*b_parsed == b);
}

TEST_CASE("parse_iso8601_utc accepts every documented fractional length", "[unit][core][time]") {
  const auto zero = ct::parse_iso8601_utc("2026-05-16T11:22:33Z");
  const auto one = ct::parse_iso8601_utc("2026-05-16T11:22:33.4Z");
  const auto two = ct::parse_iso8601_utc("2026-05-16T11:22:33.45Z");
  const auto three = ct::parse_iso8601_utc("2026-05-16T11:22:33.456Z");

  REQUIRE(zero.has_value());
  REQUIRE(one.has_value());
  REQUIRE(two.has_value());
  REQUIRE(three.has_value());

  REQUIRE(ct::format_iso8601_utc(*zero) == "2026-05-16T11:22:33.000Z");
  REQUIRE(ct::format_iso8601_utc(*one) == "2026-05-16T11:22:33.400Z");
  REQUIRE(ct::format_iso8601_utc(*two) == "2026-05-16T11:22:33.450Z");
  REQUIRE(ct::format_iso8601_utc(*three) == "2026-05-16T11:22:33.456Z");
}

TEST_CASE("parse_iso8601_utc rejects empty input with invalid_argument", "[unit][core][time]") {
  const auto r = ct::parse_iso8601_utc("");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().kind() == ErrorKind::invalid_argument);
}

TEST_CASE("parse_iso8601_utc rejects malformed shapes with parsing", "[unit][core][time]") {
  const std::string rejects[] = {
      "2026-05-16T11:22:33",         // missing Z
      "2026/05/16T11:22:33Z",        // wrong date separators
      "2026-05-16 11:22:33Z",        // missing T
      "2026-05-16T11:22:33z",        // lowercase Z
      "2026-05-16t11:22:33Z",        // lowercase T
      "2026-05-16T11:22:33.Z",       // empty fraction
      "2026-05-16T11:22:33.1234Z",   // too many fractional digits
      "2026-05-16T11:22:33.12345Z",  // even more fractional digits
      "2026-05-16T11:22:33+00:00",   // non-UTC offset
      " 2026-05-16T11:22:33Z",       // leading whitespace
      "2026-05-16T11:22:33Z ",       // trailing whitespace
      "2026-05-16T11:22:33ZZ",       // trailing junk
      "2026-13-16T11:22:33Z",        // month out of range
      "2026-05-00T11:22:33Z",        // day out of range
      "2026-05-32T11:22:33Z",        // day out of range
      "2026-05-16T24:00:00Z",        // hour out of range
      "2026-05-16T11:60:00Z",        // minute out of range
      "2026-05-16T11:22:60Z",        // second out of range
      "2026-02-30T00:00:00Z",        // invalid calendar date
      "2026-05-16T11:22:3aZ",        // non-digit field
  };

  for (const auto& s : rejects) {
    const auto r = ct::parse_iso8601_utc(s);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().kind() == ErrorKind::parsing);
  }
}

TEST_CASE("parse_iso8601_utc attaches input context on failure", "[unit][core][time]") {
  const auto r = ct::parse_iso8601_utc("2026-13-16T11:22:33Z");
  REQUIRE_FALSE(r.has_value());

  bool saw_input = false;
  for (const auto& [k, v] : r.error().context()) {
    if (k == "input" && v == "2026-13-16T11:22:33Z") {
      saw_input = true;
    }
  }
  REQUIRE(saw_input);
}

TEST_CASE("now_utc is non-decreasing within a tight loop", "[unit][core][time]") {
  Time prev = ct::now_utc();
  for (int i = 0; i < 32; ++i) {
    const Time cur = ct::now_utc();
    REQUIRE(prev <= cur);
    prev = cur;
  }
}

TEST_CASE("Time ordering follows underlying chrono time_point", "[unit][core][time]") {
  using namespace std::chrono;
  const sys_days base{year{2026} / May / 16};

  const Time a{base + hours{1}};
  const Time b{base + hours{2}};
  const Time a_copy{base + hours{1}};

  REQUIRE(a < b);
  REQUIRE(b > a);
  REQUIRE(a == a_copy);
  REQUIRE(a != b);
}
