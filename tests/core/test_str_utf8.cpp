// tests/core/test_str_utf8.cpp — RFC-3629 validator + counter + truncator.

#include <optional>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/str.hpp>

using namespace std::string_view_literals;

namespace str = orangutan::core::str;

TEST_CASE("is_valid_utf8 accepts ASCII and valid multi-byte input", "[unit][core][str][utf8]") {
  REQUIRE(str::is_valid_utf8(""sv));
  REQUIRE(str::is_valid_utf8("hello"sv));
  REQUIRE(str::is_valid_utf8("\x7F"sv));
  // U+00A9 © = 0xC2 0xA9
  REQUIRE(str::is_valid_utf8("\xC2\xA9"sv));
  // U+4E2D 中 = 0xE4 0xB8 0xAD
  REQUIRE(str::is_valid_utf8("\xE4\xB8\xAD"sv));
  // U+1F600 😀 = 0xF0 0x9F 0x98 0x80
  REQUIRE(str::is_valid_utf8("\xF0\x9F\x98\x80"sv));
  // Mixed run.
  REQUIRE(str::is_valid_utf8("hello \xC2\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80"sv));
}

TEST_CASE("is_valid_utf8 rejects overlong encodings", "[unit][core][str][utf8]") {
  // 0xC0 0x80 is the classic "overlong NUL"; lead bytes 0xC0/0xC1 are
  // forbidden by RFC 3629 because the same code point can be encoded in
  // fewer bytes.
  REQUIRE_FALSE(str::is_valid_utf8("\xC0\x80"sv));
  REQUIRE_FALSE(str::is_valid_utf8("\xC1\xBF"sv));
  // Overlong 3-byte (would encode U+007F): 0xE0 0x81 0xBF — first
  // continuation must be 0xA0..0xBF for 0xE0 leads.
  REQUIRE_FALSE(str::is_valid_utf8("\xE0\x81\xBF"sv));
  // Overlong 4-byte (would encode U+FFFF): 0xF0 0x80 0x80 0x80 — first
  // continuation must be 0x90..0xBF for 0xF0 leads.
  REQUIRE_FALSE(str::is_valid_utf8("\xF0\x80\x80\x80"sv));
}

TEST_CASE("is_valid_utf8 rejects UTF-16 surrogate code points", "[unit][core][str][utf8]") {
  // 0xED 0xA0 0x80 encodes U+D800 (high surrogate) — forbidden by RFC 3629.
  REQUIRE_FALSE(str::is_valid_utf8("\xED\xA0\x80"sv));
  // 0xED 0xBF 0xBF encodes U+DFFF (low surrogate) — forbidden.
  REQUIRE_FALSE(str::is_valid_utf8("\xED\xBF\xBF"sv));
  // 0xED 0x9F 0xBF encodes U+D7FF which is *not* a surrogate; accepted.
  REQUIRE(str::is_valid_utf8("\xED\x9F\xBF"sv));
}

TEST_CASE("is_valid_utf8 rejects out-of-range and stray bytes", "[unit][core][str][utf8]") {
  // Lone continuation byte.
  REQUIRE_FALSE(str::is_valid_utf8("\x80"sv));
  // Truncated 2-byte sequence.
  REQUIRE_FALSE(str::is_valid_utf8("\xC2"sv));
  // Truncated 3-byte sequence.
  REQUIRE_FALSE(str::is_valid_utf8("\xE4\xB8"sv));
  // Truncated 4-byte sequence.
  REQUIRE_FALSE(str::is_valid_utf8("\xF0\x9F\x98"sv));
  // 0xF5..0xFF are never valid leads (would exceed U+10FFFF).
  REQUIRE_FALSE(str::is_valid_utf8("\xF5\x80\x80\x80"sv));
  REQUIRE_FALSE(str::is_valid_utf8("\xFF"sv));
  // 0xF4 0x90 ... would encode U+110000, beyond U+10FFFF.
  REQUIRE_FALSE(str::is_valid_utf8("\xF4\x90\x80\x80"sv));
  // 0xF4 0x8F 0xBF 0xBF encodes U+10FFFF exactly; accepted.
  REQUIRE(str::is_valid_utf8("\xF4\x8F\xBF\xBF"sv));
}

TEST_CASE("count_code_points counts well-formed input", "[unit][core][str][utf8]") {
  REQUIRE(str::count_code_points(""sv) == std::optional<std::size_t>{0});
  REQUIRE(str::count_code_points("hello"sv) == std::optional<std::size_t>{5});
  REQUIRE(str::count_code_points("\xC2\xA9"sv) == std::optional<std::size_t>{1});
  REQUIRE(str::count_code_points("\xE4\xB8\xAD\xE6\x96\x87"sv) == std::optional<std::size_t>{2});
  REQUIRE(str::count_code_points("a\xF0\x9F\x98\x80"sv) == std::optional<std::size_t>{2});
}

TEST_CASE("count_code_points reports nullopt for invalid input", "[unit][core][str][utf8]") {
  REQUIRE_FALSE(str::count_code_points("\x80"sv).has_value());
  REQUIRE_FALSE(str::count_code_points("\xC2"sv).has_value());
  REQUIRE_FALSE(str::count_code_points("\xED\xA0\x80"sv).has_value());
}

TEST_CASE("truncate_to_code_point returns the input when shorter than the budget", "[unit][core][str][utf8]") {
  REQUIRE(str::truncate_to_code_point(""sv, 10) == ""sv);
  REQUIRE(str::truncate_to_code_point("abc"sv, 10) == "abc"sv);
  REQUIRE(str::truncate_to_code_point("abc"sv, 3) == "abc"sv);
}

TEST_CASE("truncate_to_code_point preserves UTF-8 boundaries", "[unit][core][str][utf8]") {
  // "中文" = 0xE4 0xB8 0xAD 0xE6 0x96 0x87 (6 bytes, 2 code points).
  constexpr auto kZhongWen = "\xE4\xB8\xAD\xE6\x96\x87"sv;
  REQUIRE(str::truncate_to_code_point(kZhongWen, 6) == kZhongWen);
  REQUIRE(str::truncate_to_code_point(kZhongWen, 5) == "\xE4\xB8\xAD"sv);
  REQUIRE(str::truncate_to_code_point(kZhongWen, 4) == "\xE4\xB8\xAD"sv);
  REQUIRE(str::truncate_to_code_point(kZhongWen, 3) == "\xE4\xB8\xAD"sv);
  REQUIRE(str::truncate_to_code_point(kZhongWen, 2) == ""sv);
  REQUIRE(str::truncate_to_code_point(kZhongWen, 0) == ""sv);
}

TEST_CASE("truncate_to_code_point handles 4-byte code points", "[unit][core][str][utf8]") {
  constexpr auto kEmoji = "a\xF0\x9F\x98\x80"sv;  // 5 bytes, 2 code points.
  REQUIRE(str::truncate_to_code_point(kEmoji, 5) == kEmoji);
  REQUIRE(str::truncate_to_code_point(kEmoji, 4) == "a"sv);
  REQUIRE(str::truncate_to_code_point(kEmoji, 1) == "a"sv);
  REQUIRE(str::truncate_to_code_point(kEmoji, 0) == ""sv);
}
