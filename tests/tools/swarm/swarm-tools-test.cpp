#include "tools/swarm/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "swarm/mailbox.hpp"
#include "swarm/team-manager.hpp"
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

    TEST_CASE("team_create tool creates a team when team manager is available", "[tools][swarm]") {
        ToolRegistry registry;
        swarm::TeamManager team_manager(":memory:");

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .team_manager = &team_manager,
        };

        register_swarm_tools(registry, &context);

        auto result = registry.execute(ToolUse("create-1", "team_create",
                                               {
                                                   {"name", "test-team"},
                                                   {"description", "A test team"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["created"] == true);
        CHECK(json["name"] == "test-team");
        REQUIRE(json.contains("team_id"));
        CHECK(team_manager.find_team(json["team_id"].get<std::string>()).has_value());
    }

    TEST_CASE("team_create tool returns an error when team manager is unavailable", "[tools][swarm]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_swarm_tools(registry, &context);

        auto result = registry.execute(ToolUse("create-2", "team_create",
                                               {
                                                   {"name", "test-team"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["created"] == false);
        REQUIRE(json["error"].is_string());
        CHECK(json["error"].get<std::string>() == "Team manager is not available");
    }

    TEST_CASE("team_delete tool returns an error when team manager is unavailable", "[tools][swarm]") {
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
        CHECK(json["deleted"] == false);
        REQUIRE(json["error"].is_string());
        CHECK(json["error"].get<std::string>() == "Team manager is not available");
    }

    TEST_CASE("team_delete sends shutdown requests before deleting the team", "[tools][swarm]") {
        ToolRegistry registry;
        swarm::TeamManager team_manager(":memory:");
        swarm::AgentMailbox mailbox(":memory:");

        auto team = team_manager.create_team("test-team", "A test team", "lead");
        team_manager.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id});
        team_manager.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "explorer", .team_id = team.id});

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_swarm_tools(registry, &context);

        auto result = registry.execute(ToolUse("delete-2", "team_delete",
                                               {
                                                   {"team_id", team.id},
                                                   {"grace_period_ms", 0},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["deleted"] == true);
        CHECK(json["shutdown_requests_sent"] == 2);

        auto worker1_messages = mailbox.poll(team.id, "worker1");
        auto worker2_messages = mailbox.poll(team.id, "worker2");
        REQUIRE(worker1_messages.size() == 1);
        REQUIRE(worker2_messages.size() == 1);
        CHECK(worker1_messages.front().from == "lead");
        CHECK(worker2_messages.front().from == "lead");
        CHECK(worker1_messages.front().type == message_type::shutdown_request);
        CHECK(worker2_messages.front().type == message_type::shutdown_request);
        CHECK(worker1_messages.front().text == "Team shutdown requested");

        CHECK_FALSE(team_manager.find_team(team.id).has_value());
        CHECK(team_manager.list_members(team.id).empty());
    }

} // namespace
