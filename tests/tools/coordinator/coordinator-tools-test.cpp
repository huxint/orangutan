#include "tools/coordinator/register.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "coordinator/coordinator-manager.hpp"
#include "swarm/mailbox.hpp"
#include "swarm/team-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <string>
#include <thread>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    TEST_CASE("Coordinator tools are not registered without context", "[tools][coordinator]") {
        ToolRegistry registry;
        register_coordinator_tools(registry, nullptr);

        auto defs = registry.definitions();
        CHECK(defs.empty());
    }

    TEST_CASE("Coordinator tools registration", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto defs = registry.definitions();

        // All coordinator tools are deferred, so definitions() returns empty.
        // But they should be findable and executable.

        SECTION("registers agent_spawn tool") {
            const auto *def = registry.find_definition("agent_spawn");
            REQUIRE(def != nullptr);

            CHECK(def->name == "agent_spawn");
            CHECK(def->description == "Spawn a worker agent to handle a delegated task. The agent will run asynchronously and report results when complete.");
            CHECK(def->input_schema == nlohmann::json{
                                           {"type", "object"},
                                           {"properties",
                                            {
                                                {"agent_key", {{"type", "string"}, {"description", "The agent type to spawn (e.g. general-purpose, explorer, planner)"}}},
                                                {"prompt", {{"type", "string"}, {"description", "The task description and instructions for the agent"}}},
                                                {"name", {{"type", "string"}, {"description", "Optional human-readable name for this agent instance"}}},
                                                {"team", {{"type", "string"}, {"description", "Optional team ID to assign this agent to"}}},
                                            }},
                                           {"required", nlohmann::json::array({"agent_key", "prompt"})},
                                       });
        }

        SECTION("registers agent_send_message tool") {
            const auto *def = registry.find_definition("agent_send_message");
            REQUIRE(def != nullptr);

            CHECK(def->name == "agent_send_message");
            CHECK(def->description == "Send a message to a running agent. Can address by run_id or agent name.");
            CHECK(def->input_schema == nlohmann::json{
                                           {"type", "object"},
                                           {"properties",
                                            {
                                                {"run_id", {{"type", "string"}, {"description", "The run ID of the target agent"}}},
                                                {"to", {{"type", "string"}, {"description", "The agent name to send to (alternative to run_id)"}}},
                                                {"text", {{"type", "string"}, {"description", "The message text to send"}}},
                                            }},
                                           {"required", nlohmann::json::array({"text"})},
                                       });
        }

        SECTION("registers agent_stop tool") {
            const auto *def = registry.find_definition("agent_stop");
            REQUIRE(def != nullptr);

            CHECK(def->name == "agent_stop");
            CHECK(def->description == "Stop a running agent. The agent will be given a chance to clean up before being terminated.");
            CHECK(def->input_schema == nlohmann::json{
                                           {"type", "object"},
                                           {"properties", {{"run_id", {{"type", "string"}, {"description", "The run ID of the agent to stop"}}}}},
                                           {"required", nlohmann::json::array({"run_id"})},
                                       });
        }

        SECTION("has deferred tools") {
            CHECK(registry.has_deferred_tools());
        }

        manager.shutdown();
    }

    TEST_CASE("agent_spawn tool returns coordinator spawn result", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("spawn-1", "agent_spawn",
                                               {
                                                   {"agent_key", "general-purpose"},
                                                   {"prompt", "test task"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["accepted"] == true);
        CHECK(json["status"] == "running");
        REQUIRE(json.contains("run_id"));
        CHECK_FALSE(json["run_id"].get<std::string>().empty());

        manager.shutdown();
    }

    TEST_CASE("agent_spawn rejects agent outside team_agents list", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .team_agents = {"explorer"},
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("spawn-2", "agent_spawn",
                                               {
                                                   {"agent_key", "general-purpose"},
                                                   {"prompt", "test task"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["accepted"] == false);
        CHECK(json["error"].get<std::string>().contains("allowed team_agents"));

        manager.shutdown();
    }

    TEST_CASE("agent_spawn registers spawned member with team", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);
        swarm::TeamManager team_manager(":memory:");
        auto team = team_manager.create_team("research", "Research team", "lead");

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .team_manager = &team_manager,
            .team_agents = {"general-purpose"},
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("spawn-3", "agent_spawn",
                                               {
                                                   {"agent_key", "general-purpose"},
                                                   {"prompt", "test task"},
                                                   {"team", team.name},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        REQUIRE(json["accepted"] == true);

        const auto members = team_manager.list_members(team.id);
        REQUIRE(members.size() == 1);
        CHECK(members.front().agent_key == "general-purpose");
        CHECK(members.front().agent_id == json["run_id"].get<std::string>());

        manager.shutdown();
    }

    TEST_CASE("agent_send_message tool validates missing recipient", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("msg-1", "agent_send_message",
                                               {
                                                   {"text", "hello agent"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["sent"] == false);
        REQUIRE(json["error"].is_string());
        CHECK(json["error"].get<std::string>() == "Either run_id or to must be specified");

        manager.shutdown();
    }

    TEST_CASE("agent_stop tool reports not found status for unknown run", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "test-agent",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("stop-1", "agent_stop",
                                               {
                                                   {"run_id", "run-12345-0"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["stopped"] == true);
        CHECK(json["status"] == "not_found");

        manager.shutdown();
    }

    TEST_CASE("agent_send_message returns an error for unknown run_id", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("msg-unknown", "agent_send_message",
                                               {
                                                   {"run_id", "run-missing"},
                                                   {"text", "hello"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["sent"] == false);
        CHECK(json["error"] == "Unknown run_id: run-missing");

        manager.shutdown();
    }

    TEST_CASE("agent_send_message delivers direct team mailbox message", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(2);
        swarm::TeamManager team_manager(":memory:");
        swarm::AgentMailbox mailbox(":memory:");
        auto team = team_manager.create_team("research", "Research team", "lead");
        team_manager.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "explorer", .team_id = team.id});

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "general-purpose",
            .agent_name = "worker1",
            .team_id = team.id,
            .coordinator_manager = &manager,
            .team_manager = &team_manager,
            .mailbox = &mailbox,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("msg-2", "agent_send_message",
                                               {
                                                   {"to", "worker2"},
                                                   {"text", "hello worker2"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["sent"] == true);

        const auto messages = mailbox.poll(team.id, "worker2");
        REQUIRE(messages.size() == 1);
        CHECK(messages.front().from == "worker1");
        CHECK(messages.front().text == "hello worker2");

        manager.shutdown();
    }

    TEST_CASE("agent_send_message delivers mailbox message by run_id", "[tools][coordinator]") {
        ToolRegistry registry;
        coordinator::CoordinatorManager manager(1);
        swarm::AgentMailbox mailbox(":memory:");
        manager.set_environment({
            .mailbox = &mailbox,
        });
        manager.set_worker_runtime_factory([](const coordinator::AgentSpawnRequest &) {
            struct WaitingWorker final : coordinator::WorkerRuntime {
                std::string run(const std::string &, std::stop_token stop_token) override {
                    for (int i = 0; i < 50; ++i) {
                        if (stop_token.stop_requested()) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    return "done";
                }
            };
            return std::make_unique<WaitingWorker>();
        });

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        const auto spawned = manager.spawn({
            .agent_key = "general-purpose",
            .agent_name = "worker-a",
            .task_prompt = "wait",
            .team_id = "team-1",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(spawned.accepted);

        auto result = registry.execute(ToolUse("msg-run-id", "agent_send_message",
                                               {
                                                   {"run_id", spawned.run_id},
                                                   {"text", "hello by run id"},
                                               }));

        CHECK_FALSE(result.is_error);
        auto json = nlohmann::json::parse(result.content);
        CHECK(json["sent"] == true);

        const auto messages = mailbox.poll("team-1", "worker-a");
        REQUIRE(messages.size() == 1);
        CHECK(messages.front().from == "lead");
        CHECK(messages.front().text == "hello by run id");

        manager.stop(spawned.run_id);
        manager.shutdown();
    }

    TEST_CASE("coordinator tools can shutdown while runtime notification callback is active", "[tools][coordinator]") {
        using namespace std::chrono_literals;

        ToolRegistry registry;
        coordinator::CoordinatorManager manager(1);
        std::atomic<bool> callback_started{false};

        manager.register_runtime_notification_handler("test-runtime", [&callback_started](const std::string &) -> std::optional<std::string> {
            callback_started.store(true);
            std::this_thread::sleep_for(300ms);
            return std::nullopt;
        });

        manager.set_worker_runtime_factory([](const coordinator::AgentSpawnRequest &) {
            struct ImmediateWorker final : coordinator::WorkerRuntime {
                std::string run(const std::string &, std::stop_token) override {
                    return "ok";
                }
            };

            return std::make_unique<ImmediateWorker>();
        });

        ToolRuntimeContext context{
            .runtime_key = "test-runtime",
            .agent_key = "lead-agent",
            .agent_name = "lead",
            .coordinator_manager = &manager,
            .coordinator_mode = true,
        };

        register_coordinator_tools(registry, &context);

        auto result = registry.execute(ToolUse("spawn-race", "agent_spawn",
                                               {
                                                   {"agent_key", "general-purpose"},
                                                   {"prompt", "trigger callback"},
                                               }));
        CHECK_FALSE(result.is_error);

        bool observed_callback = false;
        for (int i = 0; i < 100; ++i) {
            if (callback_started.load()) {
                observed_callback = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(observed_callback);

        manager.shutdown();
    }

} // namespace
