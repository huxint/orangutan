#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "tools/registry/op-tool-support.hpp"
#include "tools/registry/schema-fragments.hpp"
#include "tools/registry/contextual-tool-group.hpp"
#include "tools/registry/tool-dispatch.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "tools/registry/tool-spec-builder.hpp"

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRuntimeContext make_context(base::origin origin = base::origin::cli) {
        ToolRuntimeContext context;
        context.runtime_key = "runtime:test";
        context.agent_key = "agent:test";
        context.runtime_origin = origin;
        return context;
    }

} // namespace

TEST_CASE("tool_spec_builder: builds executable tool with passthrough flags", "[tools][registry][abstractions]") {
    ToolRegistry registry;

    auto spec = tool_spec_builder("demo_tool")
                    .description("demo helper tool")
                    .input_schema(schema_fragments::empty_object())
                    .read_only(true)
                    .deferred(true)
                    .execute([](const nlohmann::json &) {
                        return std::string{"ok"};
                    });

    registry.register_tool(spec.build());

    const auto *tool = registry.find_tool("demo_tool");
    REQUIRE(tool != nullptr);
    CHECK(tool->read_only);
    CHECK(tool->deferred);

    const auto result = registry.execute(ToolUse("demo-1", "demo_tool", nlohmann::json::object()));
    CHECK_FALSE(result.is_error);
    CHECK(result.content == "ok");
}

TEST_CASE("tool_spec_builder: rejects missing execute endpoints", "[tools][registry][abstractions]") {
    CHECK_THROWS_WITH(tool_spec_builder("missing_execute").description("should fail").input_schema(schema_fragments::empty_object()).build(),
                      Catch::Matchers::ContainsSubstring("execute"));
}

TEST_CASE("contextual_tool_group: skips registration when gate fails", "[tools][registry][abstractions]") {
    ToolRegistry registry;
    auto context = make_context();

    contextual_tool_group()
        .when([](const ToolRuntimeContext &) {
            return false;
        })
        .add(tool_spec_builder("gated_tool").description("registered only when gate passes").input_schema(schema_fragments::empty_object()).execute([](const nlohmann::json &) {
            return std::string{"gated"};
        }))
        .register_into(registry, &context);

    CHECK(registry.find_definition("gated_tool") == nullptr);
}

TEST_CASE("contextual_tool_group: gate failure does not block unrelated registration", "[tools][registry][abstractions]") {
    ToolRegistry registry;
    auto context = make_context();

    contextual_tool_group()
        .when([](const ToolRuntimeContext &) {
            return false;
        })
        .add(tool_spec_builder("blocked_tool").description("blocked").input_schema(schema_fragments::empty_object()).execute([](const nlohmann::json &) {
            return std::string{"blocked"};
        }))
        .register_into(registry, &context);

    registry.register_tool(tool_spec_builder("always_tool")
                               .description("always available")
                               .input_schema(schema_fragments::empty_object())
                               .execute([](const nlohmann::json &) {
                                   return std::string{"always"};
                               })
                               .build());

    CHECK(registry.find_definition("blocked_tool") == nullptr);
    REQUIRE(registry.find_definition("always_tool") != nullptr);
}

TEST_CASE("contextual_tool_group: requires automation runtime", "[tools][registry][abstractions]") {
    ToolRegistry registry;
    auto context = make_context(base::origin::cli);

    contextual_tool_group()
        .require_automation_runtime()
        .add(tool_spec_builder("automation_only").description("automation only").input_schema(schema_fragments::empty_object()).execute([](const nlohmann::json &) {
            return std::string{"automation"};
        }))
        .register_into(registry, &context);

    CHECK(registry.find_definition("automation_only") == nullptr);
}

TEST_CASE("contextual_tool_group: requires channel origin", "[tools][registry][abstractions]") {
    ToolRegistry registry;
    auto context = make_context(base::origin::cli);

    contextual_tool_group()
        .require_channel_origin(base::origin::channel)
        .add(tool_spec_builder("automation_origin_only").description("automation origin only").input_schema(schema_fragments::empty_object()).execute([](const nlohmann::json &) {
            return std::string{"automation-origin"};
        }))
        .register_into(registry, &context);

    CHECK(registry.find_definition("automation_origin_only") == nullptr);
}

TEST_CASE("tool_dispatch: returns configured unknown-op error", "[tools][registry][abstractions]") {
    auto dispatch = tool_dispatch().op_field("op").unknown_op_error("unsupported op");

    const auto result = dispatch.run(nlohmann::json{{"op", "nope"}, {"id", "u-1"}});
    CHECK(result.is_error);
    CHECK(result.message == "unsupported op");
}

TEST_CASE("tool_dispatch: routes known op to matching handler", "[tools][registry][abstractions]") {
    auto dispatch = tool_dispatch().op_field("op").on("ping", [](const nlohmann::json &) {
        return tool_dispatch::response{"pong"};
    });

    const auto result = dispatch.run(nlohmann::json{{"op", "ping"}, {"id", "k-1"}});
    CHECK_FALSE(result.is_error);
    CHECK(result.message == "pong");
}

TEST_CASE("tool_dispatch: handles missing op with formatter error", "[tools][registry][abstractions]") {
    auto dispatch = tool_dispatch().op_field("op").missing_op_error_formatter([](std::string_view field_name) {
        return std::string{"missing required field: "} + std::string{field_name};
    });

    const auto result = dispatch.run(nlohmann::json{{"id", "m-1"}});
    CHECK(result.is_error);
    CHECK(result.message == "missing required field: op");
}

TEST_CASE("tool_dispatch: empty missing-op formatter falls back to structured default", "[tools][registry][abstractions]") {
    auto dispatch = tool_dispatch().op_field("op").missing_op_error_formatter({});

    CHECK_NOTHROW([&dispatch] {
        const auto result = dispatch.run(nlohmann::json{{"id", "m-2"}});
        CHECK(result.is_error);
        CHECK(result.message == "missing required field: op");
    }());
}

TEST_CASE("tool_dispatch: formats unknown op from formatter", "[tools][registry][abstractions]") {
    auto dispatch = tool_dispatch().op_field("op").unknown_op_error_formatter([](std::string_view op) {
        return std::string{"unsupported operation: "} + std::string{op};
    });

    const auto result = dispatch.run(nlohmann::json{{"op", "publish"}, {"id", "u-2"}});
    CHECK(result.is_error);
    CHECK(result.message == "unsupported operation: publish");
}

TEST_CASE("op_tool_support: routed_input_with_default_op preserves explicit op", "[tools][registry][abstractions]") {
    const auto input = nlohmann::json{{"op", "list"}, {"name", "demo"}};
    const auto routed = routed_input_with_default_op(input, "fallback");

    CHECK(routed.at("op") == "list");
    CHECK(routed.at("name") == "demo");
}

TEST_CASE("op_tool_support: routed_input_with_default_op injects provided default", "[tools][registry][abstractions]") {
    const auto routed = routed_input_with_default_op(nlohmann::json::object(), "list");

    CHECK(routed.at("op") == "list");
}

TEST_CASE("op_tool_support: require_id_or_name returns exact compatibility text", "[tools][registry][abstractions]") {
    const auto result = require_id_or_name(nlohmann::json::object());

    REQUIRE(result.has_value());
    CHECK(*result == "Error: id or name is required.");
}

TEST_CASE("op_tool_support: require_id returns exact compatibility text", "[tools][registry][abstractions]") {
    const auto result = require_id(nlohmann::json::object());

    REQUIRE(result.has_value());
    CHECK(*result == "Error: id is required.");
}

TEST_CASE("op_tool_support: dispatch_message returns tool_dispatch message unchanged", "[tools][registry][abstractions]") {
    const auto output = dispatch_message(tool_dispatch().on("list",
                                                            [](const nlohmann::json &) {
                                                                return tool_dispatch::response{"ok"};
                                                            }),
                                         nlohmann::json{{"op", "list"}});

    CHECK(output == "ok");
}

TEST_CASE("schema_fragments: builds op+id object schema", "[tools][registry][abstractions]") {
    const auto schema = schema_fragments::op_id_object_schema();

    CHECK(schema == nlohmann::json{
                        {"type", "object"},
                        {"properties",
                         {
                             {"op", {{"type", "string"}}},
                             {"id", {{"type", "string"}}},
                         }},
                        {"required", nlohmann::json::array({"op", "id"})},
                    });
}

TEST_CASE("schema_fragments: provides empty object and delivery fields", "[tools][registry][abstractions]") {
    const auto empty_schema = schema_fragments::empty_object();
    const auto delivery_fields = schema_fragments::delivery_fields();

    CHECK(empty_schema == nlohmann::json{
                              {"type", "object"},
                              {"properties", nlohmann::json::object()},
                          });

    CHECK(delivery_fields == nlohmann::json{
                                 {"channel", {{"type", "string"}}},
                                 {"thread_id", {{"type", "string"}}},
                             });
}

TEST_CASE("schema_fragments: compatibility-critical reusable fields preserve exact JSON", "[tools][registry][abstractions]") {
    CHECK(schema_fragments::non_negative_index_field() == nlohmann::json{{"type", "integer"}, {"minimum", 0}});
    CHECK(schema_fragments::delivery_mode_field() == nlohmann::json{{"type", "string"}, {"enum", nlohmann::json::array({"silent", "notify"})}});
    CHECK(schema_fragments::delivery_targets_field() == nlohmann::json{{"type", "array"}, {"items", {{"type", "string"}}}});
}
