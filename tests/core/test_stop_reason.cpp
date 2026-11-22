// tests/core/test_stop_reason.cpp — `StopReason` mapping and formatter.

#include <format>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/stop_reason.hpp>

using orangutan::core::enum_name;
using orangutan::core::StopReason;

TEST_CASE("enum_name covers every StopReason enumerator", "[unit][core][stop_reason]") {
  REQUIRE(enum_name(StopReason::end_turn) == "end_turn");
  REQUIRE(enum_name(StopReason::max_tokens) == "max_tokens");
  REQUIRE(enum_name(StopReason::tool_use) == "tool_use");
  REQUIRE(enum_name(StopReason::stop_sequence) == "stop_sequence");
  REQUIRE(enum_name(StopReason::cancelled) == "cancelled");
  REQUIRE(enum_name(StopReason::error) == "error");
}

TEST_CASE("StopReason formats via std::format", "[unit][core][stop_reason]") {
  REQUIRE(std::format("{}", StopReason::end_turn) == "end_turn");
  REQUIRE(std::format("stop={}", StopReason::tool_use) == "stop=tool_use");
}

TEST_CASE("enum_name falls back to 'unknown' for out-of-range StopReason", "[unit][core][stop_reason]") {
  const auto out_of_range = static_cast<StopReason>(99);
  REQUIRE(enum_name(out_of_range) == "unknown");
}
