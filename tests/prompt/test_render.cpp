#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/tool_def.hpp>
#include <oran/prompt.hpp>

namespace core = orangutan::core;
namespace prompt = orangutan::prompt;

namespace {

prompt::RenderInputs inputs_for(std::span<const core::ToolDef> tools) {
  return prompt::RenderInputs{
      .system_preamble = "system",
      .tools = tools,
      .skills_catalog = "skills",
      .memory_framing = "memory",
      .per_agent_overlay = "overlay",
  };
}

}  // namespace

TEST_CASE("Prompt fingerprints native tools without duplicating them in text", "[unit][prompt]") {
  const std::vector<core::ToolDef> tools{core::ToolDef{
      .name = "CustomLookup",
      .description = "Look up a topic in the host service.",
      .input_schema_json = R"({"type":"object","properties":{"topic_key":{"type":"string"}}})",
      .required_capabilities = {core::Capability::egress_http},
  }};
  const auto result = prompt::render(inputs_for(tools));
  CHECK(result.prefix_bytes == 138);

  REQUIRE(result.system_prompt == "system\nskills\nmemory\noverlay");
}

TEST_CASE("Prompt byte counts include only submitted text and declarations", "[unit][prompt]") {
  auto inputs = prompt::RenderInputs{};
  std::string_view expected_text;
  SECTION("empty") {}
  SECTION("sparse") {
    inputs.skills_catalog = "skills";
    inputs.per_agent_overlay = "overlay";
    expected_text = "skills\noverlay";
  }
  const auto result = prompt::render(inputs);
  CHECK(result.prefix_bytes == expected_text.size());
  CHECK(result.system_prompt == expected_text);
}

TEST_CASE("Prompt joins nonempty sections without normalizing their bytes", "[unit][prompt]") {
  const auto result = prompt::render({
      .system_preamble = " prompt\n",
      .memory_framing = "笔记\n",
      .per_agent_overlay = " ",
  });
  REQUIRE(result.system_prompt == " prompt\n\n笔记\n\n ");
  REQUIRE(result.prefix_bytes == 18);
}

TEST_CASE("Equivalent system text has the same cache identity", "[unit][prompt]") {
  const auto first = prompt::render({.system_preamble = "a", .skills_catalog = "b"});
  const auto second = prompt::render({.system_preamble = "a\nb"});
  REQUIRE(first == second);
}

TEST_CASE("Native tool changes invalidate the prefix without changing text", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context"),
                                   core::ToolDef::with_no_input("MemoryRecall", "Read saved context")};
  const auto first = prompt::render(inputs_for(tools));

  SECTION("name") {
    tools[0].name = "CustomRead";
  }
  SECTION("description") {
    tools[0].description = "Find current context";
  }
  SECTION("schema") {
    tools[0].input_schema_json = R"({"type":"object","properties":{"topic":{"type":"string"}}})";
  }
  SECTION("selection") {
    tools.pop_back();
  }
  SECTION("native declaration order") {
    std::ranges::reverse(tools);
  }

  const auto second = prompt::render(inputs_for(tools));
  REQUIRE(first.system_prompt == second.system_prompt);
  REQUIRE(first.tool_catalog_hash != second.tool_catalog_hash);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}

TEST_CASE("Native tool fingerprints preserve field boundaries", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("ab", "c")};
  const auto first = prompt::render(inputs_for(tools));
  tools[0].name = "a";
  tools[0].description = "bc";
  const auto second = prompt::render(inputs_for(tools));
  REQUIRE(first.prefix_bytes == second.prefix_bytes);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}

TEST_CASE("Dispatch capabilities do not enter model-visible cache identity", "[unit][prompt]") {
  std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context")};
  const auto first = prompt::render(inputs_for(tools));
  tools[0].required_capabilities = {core::Capability::egress_http};
  REQUIRE(prompt::render(inputs_for(tools)) == first);
}

TEST_CASE("Changed system text invalidates cache identity", "[unit][prompt]") {
  const std::vector<core::ToolDef> tools{core::ToolDef::with_no_input("CustomLookup", "Find context")};
  auto inputs = inputs_for(tools);
  const auto first = prompt::render(inputs);

  SECTION("system preamble") {
    inputs.system_preamble = "changed system";
  }
  SECTION("skills catalogue") {
    inputs.skills_catalog = "changed skills";
  }
  SECTION("memory framing") {
    inputs.memory_framing = "changed memory";
  }
  SECTION("agent overlay") {
    inputs.per_agent_overlay = "changed overlay";
  }

  const auto second = prompt::render(inputs);
  REQUIRE(first.system_prompt != second.system_prompt);
  REQUIRE(first.tool_catalog_hash == second.tool_catalog_hash);
  REQUIRE(first.prefix_hash != second.prefix_hash);
}
