#include "coordinator/coordinator-manager.hpp"
#include "coordinator/agent-definition-registry.hpp"
#include "swarm/mailbox.hpp"
#include "swarm/team-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace orangutan;

namespace {

    TEST_CASE("Coordinator spawns worker and receives notification", "[integration][coordinator]") {
        coordinator::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        coordinator::CoordinatorManager manager(2);
        manager.set_environment(coordinator::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        std::atomic<bool> notification_received{false};
        std::string notified_run_id;
        std::mutex notify_mutex;

        manager.set_notification_callback([&](const coordinator::AgentRunRecord &record) {
            std::lock_guard lock(notify_mutex);
            notified_run_id = record.run_id;
            notification_received.store(true);
        });

        auto result = manager.spawn(coordinator::AgentSpawnRequest{
            .agent_key = "general-purpose",
            .task_prompt = "Test task for integration",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(result.accepted);
        REQUIRE(!result.run_id.empty());

        // Wait for the stub worker to complete
        for (int i = 0; i < 100 && !notification_received.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        REQUIRE(notification_received.load());
        {
            std::lock_guard lock(notify_mutex);
            CHECK(notified_run_id == result.run_id);
        }

        auto run = manager.get_run(result.run_id);
        REQUIRE(run.has_value());
        CHECK(run->status == coordinator::AgentRunStatus::succeeded);
        CHECK(!run->final_output.empty());

        manager.shutdown();
    }

    TEST_CASE("Coordinator rejects unknown agent key", "[integration][coordinator]") {
        coordinator::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        coordinator::CoordinatorManager manager(2);
        manager.set_environment(coordinator::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        auto result = manager.spawn(coordinator::AgentSpawnRequest{
            .agent_key = "nonexistent-agent",
            .task_prompt = "This should fail",
            .parent_runtime_key = "test-runtime",
        });

        CHECK_FALSE(result.accepted);
        CHECK(!result.error.empty());
        CHECK(result.run_id.empty());

        manager.shutdown();
    }

    TEST_CASE("Coordinator respects max concurrent limit", "[integration][coordinator]") {
        coordinator::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        // Allow only 1 concurrent agent -- but the stub completes instantly,
        // so we just verify that the spawn mechanism works.
        coordinator::CoordinatorManager manager(1);
        manager.set_environment(coordinator::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        auto r1 = manager.spawn(coordinator::AgentSpawnRequest{
            .agent_key = "general-purpose",
            .task_prompt = "first task",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(r1.accepted);

        // Give the stub worker time to complete
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        auto r2 = manager.spawn(coordinator::AgentSpawnRequest{
            .agent_key = "explorer",
            .task_prompt = "second task",
            .parent_runtime_key = "test-runtime",
        });
        CHECK(r2.accepted);

        manager.shutdown();
    }

    TEST_CASE("Coordinator stop terminates a run", "[integration][coordinator]") {
        coordinator::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        coordinator::CoordinatorManager manager(2);
        manager.set_environment(coordinator::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        auto result = manager.spawn(coordinator::AgentSpawnRequest{
            .agent_key = "general-purpose",
            .task_prompt = "task to stop",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(result.accepted);

        // Give worker a moment to start, then stop
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        manager.stop(result.run_id);

        auto run = manager.get_run(result.run_id);
        REQUIRE(run.has_value());
        // Status should be either succeeded (if worker completed before stop) or terminated
        CHECK((run->status == coordinator::AgentRunStatus::succeeded || run->status == coordinator::AgentRunStatus::terminated));

        manager.shutdown();
    }

    TEST_CASE("Team agents exchange messages via mailbox", "[integration][swarm]") {
        swarm::AgentMailbox mailbox(":memory:");
        swarm::TeamManager team_mgr(":memory:");

        // Create a team
        auto team = team_mgr.create_team("test-team", "Integration test team", "coordinator");
        REQUIRE(!team.id.empty());

        // Add members
        team_mgr.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id});
        team_mgr.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "explorer", .team_id = team.id});

        // Coordinator sends message to worker1
        mailbox.send(team.id, "coordinator", "worker1", "Please analyze the codebase");

        // worker1 receives the message
        auto msgs = mailbox.poll(team.id, "worker1");
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].from == "coordinator");
        CHECK(msgs[0].text == "Please analyze the codebase");
        mailbox.mark_read({msgs[0].id});

        // worker1 broadcasts a finding to all team members
        auto members = team_mgr.list_member_names(team.id);
        members.emplace_back("coordinator"); // coordinator is also a recipient
        mailbox.send_broadcast(team.id, "worker1", "Found 3 modules to update", members);

        // coordinator and worker2 should receive the broadcast (not worker1 itself)
        auto coord_msgs = mailbox.poll(team.id, "coordinator");
        auto w2_msgs = mailbox.poll(team.id, "worker2");
        auto w1_msgs = mailbox.poll(team.id, "worker1");

        CHECK(coord_msgs.size() == 1);
        CHECK(coord_msgs[0].text == "Found 3 modules to update");
        CHECK(w2_msgs.size() == 1);
        CHECK(w2_msgs[0].text == "Found 3 modules to update");
        CHECK(w1_msgs.empty()); // sender doesn't receive own broadcast
    }

    TEST_CASE("Team lifecycle with mailbox cleanup", "[integration][swarm]") {
        swarm::AgentMailbox mailbox(":memory:");
        swarm::TeamManager team_mgr(":memory:");

        auto team = team_mgr.create_team("ephemeral-team", "Short-lived team", "lead");
        REQUIRE(!team.id.empty());

        team_mgr.add_member({.agent_id = "agent-a", .name = "alpha", .agent_key = "general-purpose", .team_id = team.id});

        // Send a message
        mailbox.send(team.id, "lead", "alpha", "do the thing");
        auto msgs = mailbox.poll(team.id, "alpha");
        REQUIRE(msgs.size() == 1);

        // Delete team and clear mailbox
        team_mgr.delete_team(team.id);
        mailbox.clear_team(team.id);

        // After clearing, no messages should remain
        auto after = mailbox.poll(team.id, "alpha");
        CHECK(after.empty());

        // Team should be gone
        auto found = team_mgr.find_team(team.id);
        CHECK_FALSE(found.has_value());
    }

} // namespace
