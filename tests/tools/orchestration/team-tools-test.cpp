#include "tools/orchestration/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/orchestration-manager.hpp"
#include "orchestration/team-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    TEST_CASE("team tools are not registered without context", "[tools][orchestration]") {
        ToolRegistry registry;
        register_orchestration_tools(registry, nullptr);

        auto defs = registry.definitions();
        CHECK(defs.empty());
    }

    TEST_CASE("team tools registration", "[tools][orchestration]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_orchestration_tools(registry, &context);

        SECTION("registers team_create tool") {
            const auto *def = registry.find_definition("team_create");
            REQUIRE(def != nullptr);

            CHECK(def->name == "team_create");
            CHECK(def->description == "Create a team workspace for teammates collaborating on a shared task.");
            CHECK(def->input_schema == nlohmann::json{
                                           {"type", "object"},
                                           {"properties",
                                            {
                                                {"name", {{"type", "string"}, {"description", "A unique name for the team"}}},
                                                {"description", {{"type", "string"}, {"description", "Optional description of the team's purpose"}}},
                                            }},
                                           {"required", nlohmann::json::array({"name"})},
                                       });
        }

        SECTION("registers team_delete tool") {
            const auto *def = registry.find_definition("team_delete");
            REQUIRE(def != nullptr);

            CHECK(def->name == "team_delete");
            CHECK(def->description == "Delete a team and deactivate all its members.");
            CHECK(def->input_schema ==
                  nlohmann::json{
                      {"type", "object"},
                      {"properties",
                       {
                           {"team_id", {{"type", "string"}, {"description", "The ID of the team to delete"}}},
                           {"grace_period_ms", {{"type", "integer"}, {"description", "Optional grace period in milliseconds before deleting the team"}, {"minimum", 0}}},
                       }},
                      {"required", nlohmann::json::array({"team_id"})},
                  });
        }

        SECTION("team tools are eagerly visible") {
            CHECK_FALSE(registry.has_deferred_tools());
            CHECK(orangutan::testing::has_tool_named(registry.definitions(), "team_create"));
            CHECK(orangutan::testing::has_tool_named(registry.definitions(), "team_delete"));
        }
    }

    TEST_CASE("team_create tool creates a team when team manager is available", "[tools][orchestration]") {
        ToolRegistry registry;
        orchestration::TeamManager team_manager(":memory:");

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .team_manager = &team_manager,
        };

        register_orchestration_tools(registry, &context);

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
        CHECK(team_manager.list_member_names(json["team_id"].get<std::string>()) == std::vector<std::string>{"test-agent"});
    }

    TEST_CASE("team_create tool returns an error when team manager is unavailable", "[tools][orchestration]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_orchestration_tools(registry, &context);

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

    TEST_CASE("team_delete tool returns an error when team manager is unavailable", "[tools][orchestration]") {
        ToolRegistry registry;

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
        };

        register_orchestration_tools(registry, &context);

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

    TEST_CASE("team_delete sends shutdown requests before deleting the team", "[tools][orchestration]") {
        ToolRegistry registry;
        orchestration::TeamManager team_manager(":memory:");
        orchestration::AgentMailbox mailbox(":memory:");

        auto team = team_manager.create_team("test-team", "A test team", "lead");
        team_manager.add_member({.agent_id = "agent-1", .name = "teammate1", .config_agent_key = "lead-agent", .team_id = team.id});
        team_manager.add_member({.agent_id = "agent-2", .name = "teammate2", .config_agent_key = "lead-agent", .team_id = team.id});

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_orchestration_tools(registry, &context);

        auto result = registry.execute(ToolUse("delete-2", "team_delete",
                                               {
                                                   {"team_id", team.id},
                                                   {"grace_period_ms", 0},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["deleted"] == true);
        CHECK(json["shutdown_requests_sent"] == 2);

        auto teammate1_messages = mailbox.poll(team.id, "teammate1");
        auto teammate2_messages = mailbox.poll(team.id, "teammate2");
        REQUIRE(teammate1_messages.size() == 1);
        REQUIRE(teammate2_messages.size() == 1);
        CHECK(teammate1_messages.front().from == "lead");
        CHECK(teammate2_messages.front().from == "lead");
        CHECK(teammate1_messages.front().type == message_type::shutdown_request);
        CHECK(teammate2_messages.front().type == message_type::shutdown_request);
        CHECK(teammate1_messages.front().text == "Team shutdown requested");

        CHECK_FALSE(team_manager.find_team(team.id).has_value());
        CHECK(team_manager.list_members(team.id).empty());
    }

    TEST_CASE("team_delete does not block for grace_period_ms when no runs are active", "[tools][orchestration]") {
        ToolRegistry registry;
        orchestration::TeamManager team_manager(":memory:");
        orchestration::AgentMailbox mailbox(":memory:");

        auto team = team_manager.create_team("graceful-team", "A test team", "lead");
        team_manager.add_member({.agent_id = "agent-1", .name = "teammate1", .config_agent_key = "lead-agent", .team_id = team.id});

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_orchestration_tools(registry, &context);

        const auto start = std::chrono::steady_clock::now();
        auto result = registry.execute(ToolUse("delete-grace", "team_delete",
                                               {
                                                   {"team_id", team.id},
                                                   {"grace_period_ms", 50},
                                               }));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        CHECK_FALSE(result.is_error);
        CHECK(elapsed < std::chrono::milliseconds{40});

        auto json = nlohmann::json::parse(result.content);
        CHECK(json["deleted"] == true);
        CHECK(json["grace_period_ms"] == 50);
        CHECK(json["stopped_runs"] == 0);
        CHECK_FALSE(team_manager.find_team(team.id).has_value());
        CHECK(team_manager.list_members(team.id).empty());
    }

    TEST_CASE("team_delete stops active team runs through orchestration manager", "[tools][orchestration]") {
        ToolRegistry registry;
        orchestration::OrchestrationManager manager(1);
        orchestration::TeamManager team_manager(":memory:");
        orchestration::AgentMailbox mailbox(":memory:");

        auto team = team_manager.create_team("runtime-team", "A test team", "lead");
        manager.set_teammate_runtime_factory([](const orchestration::AgentSpawnRequest &) {
            struct IdleTeammate final : orchestration::TeammateRuntime {
                std::string run(const std::string &, std::stop_token) override {
                    return "ready";
                }

                [[nodiscard]]
                auto can_receive_followups() const -> bool override {
                    return true;
                }
            };

            return std::make_unique<IdleTeammate>();
        });
        manager.set_environment({.mailbox = &mailbox, .team_manager = &team_manager});

        const auto spawned = manager.spawn({
            .name = "teammate1",
            .task = "wait",
            .team_id = team.id,
        });
        REQUIRE(spawned.accepted);
        team_manager.add_member({.agent_id = spawned.run_id, .name = "teammate1", .config_agent_key = "lead-agent", .team_id = team.id});

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .orchestration_manager = &manager,
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_orchestration_tools(registry, &context);

        auto result = registry.execute(ToolUse("delete-runtime-team", "team_delete",
                                               {
                                                   {"team_id", team.id},
                                                   {"grace_period_ms", 0},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["deleted"] == true);
        CHECK(json["stopped_runs"] == 1);

        const auto run = manager.get_run(spawned.run_id);
        REQUIRE(run.has_value());
        CHECK(run->status == orchestration::run_status::terminated);
        CHECK_FALSE(team_manager.find_team(team.id).has_value());

        manager.shutdown();
    }

    TEST_CASE("team_delete waits up to grace_period_ms for running team runs", "[tools][orchestration]") {
        ToolRegistry registry;
        orchestration::OrchestrationManager manager(1);
        orchestration::TeamManager team_manager(":memory:");
        orchestration::AgentMailbox mailbox(":memory:");
        std::atomic<bool> run_started{false};

        auto team = team_manager.create_team("running-team", "A test team", "lead");
        manager.set_teammate_runtime_factory([&run_started](const orchestration::AgentSpawnRequest &) {
            struct BlockingTeammate final : orchestration::TeammateRuntime {
                std::atomic<bool> *run_started = nullptr;

                std::string run(const std::string &, std::stop_token stop_token) override {
                    run_started->store(true);
                    while (!stop_token.stop_requested()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    return "stopped";
                }
            };

            auto teammate = std::make_unique<BlockingTeammate>();
            teammate->run_started = &run_started;
            return teammate;
        });
        manager.set_environment({.mailbox = &mailbox, .team_manager = &team_manager});

        const auto spawned = manager.spawn({
            .name = "teammate1",
            .task = "block",
            .team_id = team.id,
        });
        REQUIRE(spawned.accepted);
        team_manager.add_member({.agent_id = spawned.run_id, .name = "teammate1", .config_agent_key = "lead-agent", .team_id = team.id});

        for (int i = 0; i < 100 && !run_started.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(run_started.load());

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .orchestration_manager = &manager,
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_orchestration_tools(registry, &context);

        const auto start = std::chrono::steady_clock::now();
        auto result = registry.execute(ToolUse("delete-running-team", "team_delete",
                                               {
                                                   {"team_id", team.id},
                                                   {"grace_period_ms", 50},
                                               }));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);

        CHECK_FALSE(result.is_error);
        CHECK(elapsed < std::chrono::milliseconds{200});
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["deleted"] == true);
        CHECK(json["stopped_runs"] == 1);

        const auto run = manager.get_run(spawned.run_id);
        REQUIRE(run.has_value());
        CHECK(run->status == orchestration::run_status::terminated);

        manager.shutdown();
    }

} // namespace
