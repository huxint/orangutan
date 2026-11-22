// tests/core/test_role.cpp — `Role` mapping and formatter.

#include <format>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/role.hpp>

using orangutan::core::enum_name;
using orangutan::core::parse_enum;
using orangutan::core::Role;

TEST_CASE("enum_name covers every Role enumerator", "[unit][core][role]") {
  REQUIRE(enum_name(Role::user) == "user");
  REQUIRE(enum_name(Role::assistant) == "assistant");
  REQUIRE(enum_name(Role::system) == "system");
  REQUIRE(enum_name(Role::tool) == "tool");
}

TEST_CASE("Role formats via std::format", "[unit][core][role]") {
  REQUIRE(std::format("{}", Role::user) == "user");
  REQUIRE(std::format("{}", Role::assistant) == "assistant");
  REQUIRE(std::format("[role={}]", Role::tool) == "[role=tool]");
}

TEST_CASE("enum_name falls back to 'unknown' for out-of-range Role", "[unit][core][role]") {
  const auto out_of_range = static_cast<Role>(99);
  REQUIRE(enum_name(out_of_range) == "unknown");
}

TEST_CASE("parse_enum<Role> round-trips every enumerator", "[unit][core][role]") {
  REQUIRE(parse_enum<Role>("user") == std::optional{Role::user});
  REQUIRE(parse_enum<Role>("assistant") == std::optional{Role::assistant});
  REQUIRE(parse_enum<Role>("system") == std::optional{Role::system});
  REQUIRE(parse_enum<Role>("tool") == std::optional{Role::tool});
}

TEST_CASE("parse_enum<Role> rejects unknown spellings", "[unit][core][role]") {
  REQUIRE_FALSE(parse_enum<Role>("").has_value());
  REQUIRE_FALSE(parse_enum<Role>("User").has_value());
  REQUIRE_FALSE(parse_enum<Role>("USER").has_value());
  REQUIRE_FALSE(parse_enum<Role>("agent").has_value());
  REQUIRE_FALSE(parse_enum<Role>("unknown").has_value());
  // `enum_name`'s fallback string must not itself round-trip.
  REQUIRE_FALSE(parse_enum<Role>(enum_name(static_cast<Role>(99))).has_value());
}
