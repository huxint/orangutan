// tests/agent/test_system_preamble.cpp - stable system preamble coverage.

#include <oran/agent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace agent = orangutan::agent;

TEST_CASE("default system preamble is stable and scoped to section one", "[unit][agent][prompt]") {
  const auto first = agent::default_system_preamble();
  const auto second = agent::default_system_preamble();

  REQUIRE(first == second);
  REQUIRE(first.section_text.contains("You are Orangutan"));
  REQUIRE(first.section_text.contains("Operating principles:"));
  REQUIRE(first.section_text.contains("Response contract:"));
  REQUIRE(first.section_text.contains("Use tools for effects"));
  REQUIRE_FALSE(first.section_text.contains("Tool:"));
  REQUIRE(first.section_text.contains("Memory discipline:"));
  REQUIRE(first.section_text.contains("MemoryRecall"));
  REQUIRE(first.section_text.contains("MemoryRemember"));
  REQUIRE_FALSE(first.section_text.contains("skill"));
  REQUIRE_FALSE(first.section_text.contains("today"));
  REQUIRE_FALSE(first.section_text.contains("request id"));
  REQUIRE_FALSE(first.section_text.contains("trace id"));
}
