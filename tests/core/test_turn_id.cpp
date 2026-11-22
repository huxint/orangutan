// tests/core/test_turn_id.cpp — TurnId value-type coverage.

#include <cstddef>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/turn_id.hpp>

namespace core = orangutan::core;

TEST_CASE("TurnId is a 16-byte value and zero detection is explicit", "[unit][core][turn_id]") {
  core::TurnId id{};
  REQUIRE(id.size() == 16);
  REQUIRE(core::is_zero_turn_id(id));

  id[7] = std::byte{0x42};
  REQUIRE_FALSE(core::is_zero_turn_id(id));
}

TEST_CASE("format_turn_id_hex emits the canonical 32-char lowercase spelling", "[unit][core][turn_id]") {
  core::TurnId id{};
  REQUIRE(core::format_turn_id_hex(id) == "00000000000000000000000000000000");

  id[0] = std::byte{0x12};
  id[15] = std::byte{0x34};
  REQUIRE(core::format_turn_id_hex(id) == "12000000000000000000000000000034");

  core::TurnId all_nibbles{};
  all_nibbles[0] = std::byte{0xab};
  all_nibbles[1] = std::byte{0xcd};
  all_nibbles[2] = std::byte{0xef};
  all_nibbles[3] = std::byte{0x09};
  const auto text = core::format_turn_id_hex(all_nibbles);
  REQUIRE(text.size() == 32);
  REQUIRE(text.starts_with("abcdef09"));
  // Lowercase-only contract: the operator surface (`--trace`) rejects
  // uppercase, so the formatter must never produce it.
  REQUIRE(text.find_first_of("ABCDEF") == std::string::npos);
}

TEST_CASE("generate_turn_id returns distinct non-zero UUIDv4-shaped ids", "[unit][core][turn_id]") {
  auto first = core::generate_turn_id();
  auto second = core::generate_turn_id();
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());

  REQUIRE_FALSE(core::is_zero_turn_id(*first));
  REQUIRE_FALSE(core::is_zero_turn_id(*second));
  REQUIRE(*first != *second);

  // Version nibble 4 and variant bits 10 per RFC 4122 layout.
  REQUIRE(((*first)[6] & std::byte{0xf0}) == std::byte{0x40});
  REQUIRE(((*first)[8] & std::byte{0xc0}) == std::byte{0x80});
}
