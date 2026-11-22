// tests/tool/test_parse_input.cpp — direct unit tests for the shared
// JSON input parsing helpers used by the built-in catalog. The header
// lives under src/oran-tool/_impl/ so we reach it through a relative path;
// the indirect coverage via Registry dispatch in test_registry.cpp also
// exercises these functions, but the direct tests document the contract
// for future built-ins (e.g. the upcoming code.* family).

#include "../../src/oran-tool/_impl/parse_input.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>

namespace detail = orangutan::tool::detail;
namespace core = orangutan::core;

namespace {

[[nodiscard]] bool has_context_key(const core::Error& err, std::string_view key) {
  return std::ranges::any_of(err.context(),
                             [key](const core::Error::ContextEntry& entry) { return entry.first == key; });
}

}  // namespace

TEST_CASE("parse_input_object rejects malformed JSON with tool-prefixed detail", "[unit][tool][parse_input]") {
  const auto result = detail::parse_input_object("{not-json", "DemoTool");

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(result.error().message() == "DemoTool: input is not valid JSON");
  REQUIRE(has_context_key(result.error(), "detail"));
}

TEST_CASE("parse_input_object rejects non-object top-level JSON", "[unit][tool][parse_input]") {
  const auto array_result = detail::parse_input_object("[1,2,3]", "DemoTool");
  REQUIRE_FALSE(array_result.has_value());
  REQUIRE(array_result.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(array_result.error().message() == "DemoTool: input must be a JSON object");

  const auto string_result = detail::parse_input_object("\"a string\"", "DemoTool");
  REQUIRE_FALSE(string_result.has_value());
  REQUIRE(string_result.error().message() == "DemoTool: input must be a JSON object");
}

TEST_CASE("parse_input_object returns the parsed object on success", "[unit][tool][parse_input]") {
  const auto result = detail::parse_input_object(R"({"path":"a","count":3})", "DemoTool");

  REQUIRE(result.has_value());
  REQUIRE(result->is_object());
  REQUIRE((*result)["path"].get<std::string>() == "a");
  REQUIRE((*result)["count"].get<int>() == 3);
}

TEST_CASE("require_string_field returns the field value on success", "[unit][tool][parse_input]") {
  const auto parsed = detail::parse_input_object(R"({"path":"x.txt"})", "DemoTool");
  REQUIRE(parsed.has_value());

  const auto path = detail::require_string_field(*parsed, "DemoTool", "path");
  REQUIRE(path.has_value());
  REQUIRE(*path == "x.txt");
}

TEST_CASE("require_string_field rejects missing fields", "[unit][tool][parse_input]") {
  const auto parsed = detail::parse_input_object(R"({"other":"value"})", "DemoTool");
  REQUIRE(parsed.has_value());

  const auto path = detail::require_string_field(*parsed, "DemoTool", "path");
  REQUIRE_FALSE(path.has_value());
  REQUIRE(path.error().kind() == core::ErrorKind::invalid_argument);
  REQUIRE(path.error().message() == "DemoTool: input must include a string `path` field");
}

TEST_CASE("require_string_field rejects non-string fields", "[unit][tool][parse_input]") {
  const auto parsed = detail::parse_input_object(R"({"path":42})", "DemoTool");
  REQUIRE(parsed.has_value());

  const auto path = detail::require_string_field(*parsed, "DemoTool", "path");
  REQUIRE_FALSE(path.has_value());
  REQUIRE(path.error().message() == "DemoTool: input must include a string `path` field");
}

TEST_CASE("require_string_field is tool-name agnostic", "[unit][tool][parse_input]") {
  const auto parsed = detail::parse_input_object(R"({"content":""})", "FileWrite");
  REQUIRE(parsed.has_value());

  const auto missing = detail::require_string_field(*parsed, "FileWrite", "path");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().message() == "FileWrite: input must include a string `path` field");

  const auto empty_ok = detail::require_string_field(*parsed, "FileWrite", "content");
  REQUIRE(empty_ok.has_value());
  REQUIRE(empty_ok->empty());
}
