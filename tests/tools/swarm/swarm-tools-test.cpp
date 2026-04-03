#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    TEST_CASE("Swarm tools are not registered without context", "[tools][swarm]") {
        ToolRegistry registry;
        register_swarm_tools(registry, nullptr);

        auto defs = registry.definitions();
        CHECK(defs.empty());
    }

    TEST_CASE("Swarm tools registration", "[tools][swarm]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_swarm_tools(registry, &context);

        SECTION("registers team_create tool") {
            const auto *def = registry.find_definition("team_create");
            REQUIRE(def != nullptr);
            CHECK(!def->description.empty());
            CHECK(def->input_schema.contains("properties"));
            CHECK(def->input_schema["properties"].contains("name"));
        }

        SECTION("registers team_delete tool") {
            const auto *def = registry.find_definition("team_delete");
            REQUIRE(def != nullptr);
            CHECK(!def->description.empty());
            CHECK(def->input_schema.contains("properties"));
            CHECK(def->input_schema["properties"].contains("team_id"));
        }

        SECTION("has deferred tools") {
            CHECK(registry.has_deferred_tools());
        }
    }

    TEST_CASE("team_create tool returns stub response", "[tools][swarm]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_swarm_tools(registry, &context);

        auto result = registry.execute(ToolUse("create-1", "team_create",
                                               {
                                                   {"name", "test-team"},
                                                   {"description", "A test team"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json.contains("created"));
        CHECK(json.contains("team_id"));
        CHECK(json.contains("error"));
    }

    TEST_CASE("team_delete tool returns stub response", "[tools][swarm]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_swarm_tools(registry, &context);

        auto result = registry.execute(ToolUse("delete-1", "team_delete",
                                               {
                                                   {"team_id", "team-12345"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json.contains("deleted"));
        CHECK(json.contains("error"));
    }

} // namespace
