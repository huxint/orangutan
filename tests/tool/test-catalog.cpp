#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/tool/catalog.hpp>

namespace core = orangutan::core;
namespace tool = orangutan::tool;

TEST_CASE("Tool selection defaults to every registered definition", "[unit][tool][catalog]") {
  const std::vector<core::ToolDef> catalog{core::ToolDef::with_no_input("MemoryForget", "Forget a note"),
                                           core::ToolDef::with_no_input("CustomTool", "Host extension"),
                                           core::ToolDef::with_no_input("FileRead", "Read a file")};
  const auto selected = tool::select_tools(catalog);
  REQUIRE(selected.has_value());
  REQUIRE(*selected == std::vector<core::ToolDef>{catalog[1], catalog[2], catalog[0]});

  auto reordered = catalog;
  std::ranges::reverse(reordered);
  const auto from_reordered = tool::select_tools(reordered);
  REQUIRE(from_reordered.has_value());
  REQUIRE(*from_reordered == *selected);
}

TEST_CASE("Explicit tool selection is sorted and does not duplicate declarations", "[unit][tool][catalog]") {
  const std::vector<core::ToolDef> catalog{core::ToolDef::with_no_input("FileRead", "Read a file"),
                                           core::ToolDef::with_no_input("CustomTool", "Host extension"),
                                           core::ToolDef::with_no_input("MemoryForget", "Forget a note")};
  const std::vector<std::string> names{"MemoryForget", "CustomTool", "MemoryForget"};
  const auto selected = tool::select_tools(catalog, names);
  REQUIRE(selected.has_value());
  REQUIRE(*selected == std::vector<core::ToolDef>{catalog[1], catalog[2]});
}

TEST_CASE("Explicit empty tool selection exposes no tools", "[unit][tool][catalog]") {
  const std::vector<core::ToolDef> catalog{core::ToolDef::with_no_input("FileRead", "Read a file")};
  const auto selected = tool::select_tools(catalog, std::span<const std::string>{});
  REQUIRE(selected.has_value());
  REQUIRE(selected->empty());
}

TEST_CASE("Unknown tool selection reports the missing name", "[unit][tool][catalog]") {
  const std::vector<core::ToolDef> catalog{core::ToolDef::with_no_input("FileRead", "Read a file")};
  const std::vector<std::string> names{"FileRead", "MissingTool"};
  const auto selected = tool::select_tools(catalog, names);
  REQUIRE_FALSE(selected.has_value());
  REQUIRE(selected.error().kind() == core::ErrorKind::not_found);
  REQUIRE(std::ranges::any_of(selected.error().context(), [](const auto& entry) {
    return entry.first == "tool" && entry.second == "MissingTool";
  }));
}

TEST_CASE("Ambiguous tool catalogues fail before reaching a provider", "[unit][tool][catalog]") {
  const std::vector<core::ToolDef> catalog{core::ToolDef::with_no_input("CustomTool", "First definition"),
                                           core::ToolDef::with_no_input("CustomTool", "Conflicting definition")};
  const auto selected = tool::select_tools(catalog);
  REQUIRE_FALSE(selected.has_value());
  REQUIRE(selected.error().kind() == core::ErrorKind::invalid_argument);
}
