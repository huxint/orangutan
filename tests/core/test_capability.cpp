// tests/core/test_capability.cpp — `Capability` enumerator, parse, and formatter.

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/capability.hpp>

using orangutan::core::Capability;
using orangutan::core::enum_name;
using orangutan::core::enum_values;
using orangutan::core::parse_enum;

TEST_CASE("enum_name covers every Capability enumerator", "[unit][core][capability]") {
  REQUIRE(enum_name(Capability::read_file) == "read_file");
  REQUIRE(enum_name(Capability::write_file) == "write_file");
  REQUIRE(enum_name(Capability::edit_file) == "edit_file");
  REQUIRE(enum_name(Capability::delete_path) == "delete_path");
  REQUIRE(enum_name(Capability::list_directory) == "list_directory");
  REQUIRE(enum_name(Capability::egress_http) == "egress_http");
  REQUIRE(enum_name(Capability::egress_websocket) == "egress_websocket");
  REQUIRE(enum_name(Capability::spawn_subprocess) == "spawn_subprocess");
  REQUIRE(enum_name(Capability::signal_subprocess) == "signal_subprocess");
  REQUIRE(enum_name(Capability::external_mcp) == "external_mcp");
  REQUIRE(enum_name(Capability::runtime_loader) == "runtime_loader");
}

TEST_CASE("Capability formats via std::format", "[unit][core][capability]") {
  REQUIRE(std::format("{}", Capability::read_file) == "read_file");
  REQUIRE(std::format("[cap={}]", Capability::spawn_subprocess) == "[cap=spawn_subprocess]");
  REQUIRE(std::format("{}", Capability::runtime_loader) == "runtime_loader");
}

TEST_CASE("enum_name falls back to 'unknown' for out-of-range Capability", "[unit][core][capability]") {
  const auto out_of_range = static_cast<Capability>(99);
  REQUIRE(enum_name(out_of_range) == "unknown");
}

TEST_CASE("parse_enum<Capability> round-trips every enumerator", "[unit][core][capability]") {
  for (const auto cap : enum_values<Capability>()) {
    const auto parsed = parse_enum<Capability>(enum_name(cap));
    REQUIRE(parsed.has_value());
    REQUIRE(*parsed == cap);
  }
}

TEST_CASE("parse_enum<Capability> rejects unknown spellings", "[unit][core][capability]") {
  REQUIRE_FALSE(parse_enum<Capability>("").has_value());
  REQUIRE_FALSE(parse_enum<Capability>("Read_File").has_value());
  REQUIRE_FALSE(parse_enum<Capability>("read-file").has_value());
  REQUIRE_FALSE(parse_enum<Capability>("readfile").has_value());
  REQUIRE_FALSE(parse_enum<Capability>("network").has_value());
  // The `enum_name` fallback sentinel must not round-trip.
  REQUIRE_FALSE(parse_enum<Capability>(enum_name(static_cast<Capability>(99))).has_value());
}

TEST_CASE("enum_values<Capability> lists every enumerator exactly once", "[unit][core][capability]") {
  constexpr auto all = enum_values<Capability>();
  // The size matches the enumerator list and every entry round-trips through
  // the string mapping.
  STATIC_REQUIRE(all.size() == 11);
  for (const auto cap : all) {
    REQUIRE(enum_name(cap) != "unknown");
  }
  // The view is in declaration order; check the endpoints explicitly so a
  // reordering shows up here rather than silently moving the bench fixture.
  REQUIRE(all.front() == Capability::read_file);
  REQUIRE(all.back() == Capability::runtime_loader);
  // Spellings are unique.
  std::vector<std::string_view> spellings;
  spellings.reserve(all.size());
  for (const auto cap : all) {
    spellings.push_back(enum_name(cap));
  }
  std::ranges::sort(spellings);
  const auto duplicate = std::ranges::adjacent_find(spellings);
  REQUIRE(duplicate == spellings.end());
}
