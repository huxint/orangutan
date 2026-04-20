#include "orchestration/orchestration-manager.hpp"
#include "orchestration/agent-definition-registry.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "test-helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace orangutan;

namespace {

    TEST_CASE("orchestration manager spawns worker and receives notification", "[integration][orchestration]") {
        orchestration::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        orchestration::OrchestrationManager manager(2);
        manager.set_environment(orchestration::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });
        manager.set_worker_runtime_factory([](const orchestration::AgentSpawnRequest &) {
            struct ImmediateWorker final : orchestration::WorkerRuntime {
                std::string run(const std::string &, std::stop_token) override {
                    return "integration done";
                }
            };
            return std::make_unique<ImmediateWorker>();
        });

        std::atomic<bool> notification_received{false};
        std::string notified_run_id;
        std::mutex notify_mutex;

        manager.set_notification_callback([&](const orchestration::AgentRunRecord &record) {
            std::scoped_lock lock(notify_mutex);
            notified_run_id = record.run_id;
            notification_received.store(true);
        });

        auto result = manager.spawn(orchestration::AgentSpawnRequest{
            .agent_key = "general-purpose",
            .task_prompt = "Test task for integration",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(result.accepted);
        REQUIRE(!result.run_id.empty());

        // Wait for the worker to complete
        for (int i = 0; i < 100 && !notification_received.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        REQUIRE(notification_received.load());
        {
            std::scoped_lock lock(notify_mutex);
            CHECK(notified_run_id == result.run_id);
        }

        auto run = manager.get_run(result.run_id);
        REQUIRE(run.has_value());
        CHECK(run->status == orchestration::run_status::succeeded);
        CHECK(run->final_output == "integration done");

        manager.shutdown();
    }

    TEST_CASE("orchestration manager rejects unknown agent key", "[integration][orchestration]") {
        orchestration::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        orchestration::OrchestrationManager manager(2);
        manager.set_environment(orchestration::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        auto result = manager.spawn(orchestration::AgentSpawnRequest{
            .agent_key = "nonexistent-agent",
            .task_prompt = "This should fail",
            .parent_runtime_key = "test-runtime",
        });

        CHECK_FALSE(result.accepted);
        CHECK(!result.error.empty());
        CHECK(result.run_id.empty());

        manager.shutdown();
    }

    TEST_CASE("orchestration manager queues runs beyond max concurrent limit", "[integration][orchestration]") {
        orchestration::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        orchestration::OrchestrationManager manager(1);
        manager.set_environment(orchestration::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });

        std::atomic<bool> release_first{false};
        std::atomic<int> started{0};
        manager.set_worker_runtime_factory([&](const orchestration::AgentSpawnRequest &request) {
            struct QueuedWorker final : orchestration::WorkerRuntime {
                std::atomic<bool> *release_first = nullptr;
                std::atomic<int> *started = nullptr;
                std::string task_prompt;

                std::string run(const std::string &, std::stop_token) override {
                    started->fetch_add(1);
                    if (task_prompt == "first task") {
                        for (int i = 0; i < 100 && !release_first->load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                    return task_prompt + " complete";
                }
            };

            auto worker = std::make_unique<QueuedWorker>();
            worker->release_first = &release_first;
            worker->started = &started;
            worker->task_prompt = request.task_prompt;
            return worker;
        });

        auto r1 = manager.spawn(orchestration::AgentSpawnRequest{
            .agent_key = "general-purpose",
            .task_prompt = "first task",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(r1.accepted);

        for (int i = 0; i < 50 && started.load() < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(started.load() == 1);

        auto r2 = manager.spawn(orchestration::AgentSpawnRequest{
            .agent_key = "explorer",
            .task_prompt = "second task",
            .parent_runtime_key = "test-runtime",
        });
        REQUIRE(r2.accepted);

        auto queued = manager.get_run(r2.run_id);
        REQUIRE(queued.has_value());
        CHECK(queued->status == orchestration::run_status::queued);
        CHECK(started.load() == 1);

        release_first.store(true);

        for (int i = 0; i < 100; ++i) {
            auto run = manager.get_run(r2.run_id);
            if (run.has_value() && run->status == orchestration::run_status::succeeded) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        auto completed = manager.get_run(r2.run_id);
        REQUIRE(completed.has_value());
        CHECK(completed->status == orchestration::run_status::succeeded);
        CHECK(started.load() == 2);

        manager.shutdown();
    }

    TEST_CASE("orchestration manager stop terminates a run", "[integration][orchestration]") {
        orchestration::AgentDefinitionRegistry def_registry;
        def_registry.load_builtin_definitions();

        orchestration::OrchestrationManager manager(2);
        manager.set_environment(orchestration::AgentExecutionEnvironment{
            .definition_registry = &def_registry,
        });
        manager.set_worker_runtime_factory([](const orchestration::AgentSpawnRequest &) {
            struct SlowWorker final : orchestration::WorkerRuntime {
                std::string run(const std::string &, std::stop_token stop_token) override {
                    for (int i = 0; i < 100; ++i) {
                        if (stop_token.stop_requested()) {
                            return "stopped";
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    return "completed";
                }
            };
            return std::make_unique<SlowWorker>();
        });

        auto result = manager.spawn(orchestration::AgentSpawnRequest{
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
        CHECK((run->status == orchestration::run_status::succeeded || run->status == orchestration::run_status::terminated));

        manager.shutdown();
    }

    TEST_CASE("Team agents exchange messages via mailbox", "[integration][orchestration]") {
        orchestration::AgentMailbox mailbox(":memory:");
        orchestration::TeamManager team_mgr(":memory:");

        // Create a team
        auto team = team_mgr.create_team("test-team", "Integration test team", "leader");
        REQUIRE(!team.id.empty());

        // Add members
        team_mgr.add_member({.agent_id = "agent-1", .name = "worker1", .agent_key = "general-purpose", .team_id = team.id});
        team_mgr.add_member({.agent_id = "agent-2", .name = "worker2", .agent_key = "explorer", .team_id = team.id});

        // Leader sends message to worker1
        mailbox.send(team.id, "leader", "worker1", "Please analyze the codebase");

        // worker1 receives the message
        auto msgs = mailbox.poll(team.id, "worker1");
        REQUIRE(msgs.size() == 1);
        CHECK(msgs[0].from == "leader");
        CHECK(msgs[0].text == "Please analyze the codebase");
        mailbox.mark_read({msgs[0].id});

        // worker1 broadcasts a finding to all team members
        auto members = team_mgr.list_member_names(team.id);
        members.emplace_back("leader"); // leader is also a recipient
        mailbox.send_broadcast(team.id, "worker1", "Found 3 modules to update", members);

        // leader and worker2 should receive the broadcast (not worker1 itself)
        auto coord_msgs = mailbox.poll(team.id, "leader");
        auto w2_msgs = mailbox.poll(team.id, "worker2");
        auto w1_msgs = mailbox.poll(team.id, "worker1");

        CHECK(coord_msgs.size() == 1);
        CHECK(coord_msgs[0].text == "Found 3 modules to update");
        CHECK(w2_msgs.size() == 1);
        CHECK(w2_msgs[0].text == "Found 3 modules to update");
        CHECK(w1_msgs.empty()); // sender doesn't receive own broadcast
    }

    TEST_CASE("Team lifecycle with mailbox cleanup", "[integration][orchestration]") {
        orchestration::AgentMailbox mailbox(":memory:");
        orchestration::TeamManager team_mgr(":memory:");

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
