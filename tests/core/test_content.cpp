// tests/core/test_content.cpp — `Content` variant helpers and equality.

#include <variant>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/content.hpp>

using orangutan::core::Content;
using orangutan::core::holds_text;
using orangutan::core::holds_thinking;
using orangutan::core::holds_tool_result;
using orangutan::core::holds_tool_use;
using orangutan::core::text_view;
using orangutan::core::TextContent;
using orangutan::core::ThinkingContent;
using orangutan::core::ToolResultContent;
using orangutan::core::ToolUseContent;

TEST_CASE("Content holds_* predicates match the active alternative", "[unit][core][content]") {
  const Content text = TextContent{.text = "hello"};
  const Content thinking = ThinkingContent{.thinking = "deep", .signature = std::nullopt};
  const Content tool_use = ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"};
  const Content tool_result =
      ToolResultContent{.tool_use_id = "t1", .output = "ok", .data_json = std::nullopt, .is_error = false};

  REQUIRE(holds_text(text));
  REQUIRE_FALSE(holds_thinking(text));
  REQUIRE_FALSE(holds_tool_use(text));
  REQUIRE_FALSE(holds_tool_result(text));

  REQUIRE(holds_thinking(thinking));
  REQUIRE_FALSE(holds_text(thinking));

  REQUIRE(holds_tool_use(tool_use));
  REQUIRE_FALSE(holds_tool_result(tool_use));

  REQUIRE(holds_tool_result(tool_result));
  REQUIRE_FALSE(holds_tool_use(tool_result));
}

TEST_CASE("text_view exposes the inner string only for TextContent", "[unit][core][content]") {
  const Content text = TextContent{.text = "hello"};
  const Content tool_use = ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"};

  const auto view = text_view(text);
  REQUIRE(view.has_value());
  REQUIRE(*view == "hello");

  REQUIRE_FALSE(text_view(tool_use).has_value());
}

TEST_CASE("Content equality is member-wise", "[unit][core][content]") {
  const Content a = TextContent{.text = "hello"};
  const Content b = TextContent{.text = "hello"};
  const Content c = TextContent{.text = "world"};

  REQUIRE(a == b);
  REQUIRE_FALSE(a == c);

  const Content tool_a = ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"};
  const Content tool_b = ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"};
  const Content tool_c = ToolUseContent{.id = "t2", .name = "FileRead", .input_json = "{}"};

  REQUIRE(tool_a == tool_b);
  REQUIRE_FALSE(tool_a == tool_c);
}

TEST_CASE("Different alternatives with the same payload compare unequal", "[unit][core][content]") {
  const Content text = TextContent{.text = ""};
  const Content thinking = ThinkingContent{.thinking = "", .signature = std::nullopt};
  REQUIRE_FALSE(text == thinking);
}

TEST_CASE("ToolResultContent flags an error with member-wise compare", "[unit][core][content]") {
  const Content ok =
      ToolResultContent{.tool_use_id = "t1", .output = "fine", .data_json = std::nullopt, .is_error = false};
  const Content bad =
      ToolResultContent{.tool_use_id = "t1", .output = "fine", .data_json = std::nullopt, .is_error = true};

  REQUIRE_FALSE(ok == bad);
  REQUIRE(holds_tool_result(ok));
  REQUIRE(holds_tool_result(bad));
}

TEST_CASE("ToolResultContent preserves optional structured output", "[unit][core][content]") {
  const Content text_only =
      ToolResultContent{.tool_use_id = "t1", .output = "fine", .data_json = std::nullopt, .is_error = false};
  const Content structured = ToolResultContent{
      .tool_use_id = "t1",
      .output = "fine",
      .data_json = std::string{R"({"kind":"file_read"})"},
      .is_error = false,
  };

  REQUIRE_FALSE(text_only == structured);
  const auto& result = std::get<ToolResultContent>(structured);
  REQUIRE(result.data_json == std::optional<std::string>{R"({"kind":"file_read"})"});
}

TEST_CASE("std::visit with Overloaded set walks every alternative", "[unit][core][content]") {
  struct Overloaded {
    int operator()(const TextContent&) const {
      return 1;
    }
    int operator()(const ThinkingContent&) const {
      return 2;
    }
    int operator()(const ToolUseContent&) const {
      return 3;
    }
    int operator()(const ToolResultContent&) const {
      return 4;
    }
    int operator()(const orangutan::core::ImageContent&) const {
      return 5;
    }
  };

  REQUIRE(std::visit(Overloaded{}, Content{TextContent{.text = "x"}}) == 1);
  REQUIRE(std::visit(Overloaded{}, Content{orangutan::core::ImageContent{"image/png", "aW1hZ2U="}}) == 5);
  REQUIRE(std::visit(Overloaded{}, Content{ThinkingContent{.thinking = "x", .signature = std::nullopt}}) == 2);
  REQUIRE(std::visit(Overloaded{}, Content{ToolUseContent{.id = "1", .name = "a", .input_json = "{}"}}) == 3);
  REQUIRE(std::visit(Overloaded{},
                     Content{ToolResultContent{
                         .tool_use_id = "1",
                         .output = "y",
                         .data_json = std::nullopt,
                         .is_error = false,
                     }}) == 4);
}
