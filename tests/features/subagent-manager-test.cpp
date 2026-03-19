#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "core/providers/provider.hpp"
#include "features/subagent/subagent-manager.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <stop_token>
#include <thread>

using namespace orangutan;

namespace {

class ScriptedProvider final : public Provider {
public:
    using Step = std::function<LLMResponse(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &)>;

    explicit ScriptedProvider(std::vector<Step> steps)
    : steps_(std::move(steps)) {}

    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &, int) override {
        if (next_step_ >= steps_.size()) {
            throw std::runtime_error("no scripted response available");
        }
        return steps_[next_step_++](system_prompt, messages, tools);
    }

    std::string name() const override {
        return "scripted-provider";
    }

private:
    std::vector<Step> steps_;
    size_t next_step_ = 0;
};

const ToolDef *find_tool(const std::vector<ToolDef> &tools, const std::string &name) {
    const auto it = std::ranges::find_if(tools, [&](const ToolDef &tool) {
        return tool.name == name;
    });
    return it == tools.end() ? nullptr : &*it;
}

} // namespace

class SubagentManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_subagent_manager_test.db";
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    [[nodiscard]]
    std::array<std::string, 2> create_linked_sessions() const {
        SessionStore session_store(db_path().string());
        return {
            session_store.create_empty("test-model", "scope:parent"),
            session_store.create_empty("test-model", "scope:child"),
        };
    }

    [[nodiscard]]
    static SubagentCallerContext sample_caller_context(const std::string &parent_session_id, bool is_child_run = false) {
        return SubagentCallerContext{
            .runtime_origin = SubagentRuntimeOrigin::cli,
            .runtime_key = "runtime:cli:default",
            .agent_key = "default",
            .scope_key = "scope:parent",
            .raw_caller_id = "cli:local",
            .session_id = parent_session_id,
            .allowed_child_agents = {"coder", "reviewer"},
            .is_child_run = is_child_run,
        };
    }

    [[nodiscard]]
    static SubagentSpawnRequest sample_spawn_request(const std::string &parent_session_id, const std::string &child_session_id) {
        return SubagentSpawnRequest{
            .caller = sample_caller_context(parent_session_id),
            .child_agent_key = "coder",
            .child_session_id = child_session_id,
            .child_scope_key = "scope:child",
            .task_summary = "Investigate failing parser tests",
        };
    }

    [[nodiscard]]
    const std::filesystem::path &db_path() const {
        return db_path_;
    }

private:
    std::filesystem::path db_path_;
};

TEST_F(SubagentManagerTest, SpawnReturnsAcceptedRunAndWaitReturnsFinalResultUsingFakeWorker) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());

    auto row_existed_before_worker = false;
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &request) {
        SubagentRunStore worker_store(db_path().string());
        row_existed_before_worker = worker_store.load_run(request.run_id).has_value();
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Done",
            .final_output = "Detailed output",
        };
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));

    ASSERT_TRUE(spawn_result.accepted);
    EXPECT_FALSE(spawn_result.run_id.empty());
    EXPECT_TRUE(spawn_result.error.empty());

    const auto wait_result = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
    });

    ASSERT_EQ(wait_result.state, SubagentWaitState::completed);
    ASSERT_TRUE(wait_result.run.has_value());
    EXPECT_EQ(wait_result.run->status, SubagentRunStatus::succeeded);
    EXPECT_EQ(wait_result.run->final_summary, "Done");
    EXPECT_EQ(wait_result.run->final_output, "Detailed output");
    EXPECT_TRUE(row_existed_before_worker);

    const auto status_result = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
    ASSERT_TRUE(status_result.run.has_value());
    EXPECT_EQ(status_result.run->status, SubagentRunStatus::succeeded);
    EXPECT_EQ(status_result.run->parent_runtime_key, "runtime:cli:default");
    EXPECT_EQ(status_result.run->child_agent_key, "coder");
}

TEST_F(SubagentManagerTest, ChildCallersAreRejected) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    auto request = sample_spawn_request(parent_session_id, child_session_id);
    request.caller.is_child_run = true;

    const auto spawn_result = manager.spawn(request);

    EXPECT_FALSE(spawn_result.accepted);
    EXPECT_TRUE(spawn_result.run_id.empty());
    EXPECT_FALSE(spawn_result.error.empty());
}

TEST_F(SubagentManagerTest, AllowlistViolationsAreRejected) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    auto request = sample_spawn_request(parent_session_id, child_session_id);
    request.caller.allowed_child_agents = {"reviewer"};

    const auto spawn_result = manager.spawn(request);

    EXPECT_FALSE(spawn_result.accepted);
    EXPECT_TRUE(spawn_result.run_id.empty());
    EXPECT_FALSE(spawn_result.error.empty());
}

TEST_F(SubagentManagerTest, WaitCanTimeOutCleanly) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    std::promise<void> release_worker;
    auto release_future = release_worker.get_future().share();
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &) {
        release_future.wait();
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Done later",
            .final_output = "Delayed output",
        };
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(spawn_result.accepted);

    const auto timed_out = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::milliseconds{10},
    });
    EXPECT_EQ(timed_out.state, SubagentWaitState::timed_out);
    EXPECT_FALSE(timed_out.run.has_value());

    release_worker.set_value();

    const auto completed = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
    });
    ASSERT_EQ(completed.state, SubagentWaitState::completed);
    ASSERT_TRUE(completed.run.has_value());
    EXPECT_EQ(completed.run->status, SubagentRunStatus::succeeded);
}

TEST_F(SubagentManagerTest, RealChildInheritsCallerApprovalCallback) {
    SessionStore session_store(db_path().string());
    SubagentRunStore run_store(db_path().string());

    ToolPermissionSettings child_permissions;
    child_permissions.sandbox_mode = ToolSandboxMode::disabled;
    child_permissions.shell_approval = ToolApprovalPolicy::ask;

    std::unordered_map<std::string, SubagentChildRuntimeConfig> child_configs;
    child_configs.emplace("coder", SubagentChildRuntimeConfig{
                                       .agent_key = "coder",
                                       .provider_name = "child-provider",
                                       .api_key = "unused",
                                       .model = "child-model",
                                       .base_url = "https://example.test",
                                       .system_prompt = "Child base prompt.",
                                       .workspace_root = std::filesystem::temp_directory_path().string(),
                                       .permissions = child_permissions,
                                   });

    bool prompted = false;
    SubagentManager manager(run_store, SubagentExecutionEnvironment{
                                          .agent_configs = &child_configs,
                                          .session_store = &session_store,
                                          .memory_store = nullptr,
                                          .provider_factory =
                                              [&](const SubagentChildRuntimeConfig &) {
                                                  auto steps = std::vector<ScriptedProvider::Step>{
                                                      [&](const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                                                          return LLMResponse{
                                                              .stop_reason = "tool_use",
                                                              .content = {ToolUseBlock{.id = "child-shell", .name = "shell", .input = {{"command", "echo child"}}}},
                                                          };
                                                      },
                                                      [&](const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                                                          return LLMResponse{
                                                              .stop_reason = "end_turn",
                                                              .content = {TextBlock{.text = "child completed"}},
                                                          };
                                                      },
                                                  };
                                                  return std::make_unique<ScriptedProvider>(std::move(steps));
                                              },
                                      });

    auto caller = sample_caller_context(std::string{});
    caller.session_id = std::nullopt;
    caller.allowed_child_agents = {"coder"};
    caller.approval_callback = [&prompted](const ToolUseBlock &call, const std::string &prompt_text) {
        prompted = true;
        EXPECT_EQ(call.name, "shell");
        EXPECT_NE(prompt_text.find("echo child"), std::string::npos);
        return true;
    };

    const auto spawn_result = manager.spawn(SubagentSpawnRequest{
        .caller = caller,
        .child_agent_key = "coder",
        .task_summary = "Run a child shell command",
    });
    ASSERT_TRUE(spawn_result.accepted);

    const auto wait_result = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
        .caller = caller,
    });
    ASSERT_EQ(wait_result.state, SubagentWaitState::completed);
    ASSERT_TRUE(wait_result.run.has_value());
    EXPECT_EQ(wait_result.run->status, SubagentRunStatus::succeeded);
    EXPECT_TRUE(prompted);
}

TEST_F(SubagentManagerTest, RealChildUsesConfiguredHashlineEditMode) {
    SessionStore session_store(db_path().string());
    SubagentRunStore run_store(db_path().string());

    std::unordered_map<std::string, SubagentChildRuntimeConfig> child_configs;
    child_configs.emplace("coder", SubagentChildRuntimeConfig{
                                       .agent_key = "coder",
                                       .provider_name = "child-provider",
                                       .api_key = "unused",
                                       .model = "child-model",
                                       .base_url = "https://example.test",
                                       .system_prompt = "Child base prompt.",
                                       .workspace_root = std::filesystem::temp_directory_path().string(),
                                       .edit_mode = "hashline",
                                   });

    SubagentManager manager(run_store, SubagentExecutionEnvironment{
                                          .agent_configs = &child_configs,
                                          .session_store = &session_store,
                                          .memory_store = nullptr,
                                          .provider_factory =
                                              [&](const SubagentChildRuntimeConfig &) {
                                                  auto steps = std::vector<ScriptedProvider::Step>{
                                                      [&](const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &tools) -> LLMResponse {
                                                          const auto *edit_tool = find_tool(tools, "edit");
                                                          EXPECT_NE(edit_tool, nullptr);
                                                          if (edit_tool != nullptr) {
                                                              EXPECT_NE(edit_tool->description.find("hash"), std::string::npos);
                                                              EXPECT_TRUE(edit_tool->input_schema.contains("properties"));
                                                              if (edit_tool->input_schema.contains("properties")) {
                                                                  EXPECT_TRUE(edit_tool->input_schema["properties"].contains("edits"));
                                                                  EXPECT_FALSE(edit_tool->input_schema["properties"].contains("patch"));
                                                              }
                                                          }

                                                          const auto *read_tool = find_tool(tools, "read");
                                                          EXPECT_NE(read_tool, nullptr);
                                                          if (read_tool != nullptr) {
                                                              EXPECT_NE(read_tool->description.find("line numbers"), std::string::npos);
                                                          }

                                                          return LLMResponse{
                                                              .stop_reason = "end_turn",
                                                              .content = {TextBlock{.text = "child completed"}},
                                                          };
                                                      },
                                                  };
                                                  return std::make_unique<ScriptedProvider>(std::move(steps));
                                              },
                                      });

    auto caller = sample_caller_context(std::string{});
    caller.session_id = std::nullopt;
    caller.allowed_child_agents = {"coder"};

    const auto spawn_result = manager.spawn(SubagentSpawnRequest{
        .caller = caller,
        .child_agent_key = "coder",
        .task_summary = "Inspect available tools",
    });
    ASSERT_TRUE(spawn_result.accepted);

    const auto wait_result = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
        .caller = caller,
    });
    ASSERT_EQ(wait_result.state, SubagentWaitState::completed);
    ASSERT_TRUE(wait_result.run.has_value());
    EXPECT_EQ(wait_result.run->status, SubagentRunStatus::succeeded);
}

TEST_F(SubagentManagerTest, ConstructorDoesNotAbandonStaleRunsAndExplicitCleanupDoes) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    run_store.create_run(SubagentRunCreateParams{
        .run_id = "stale-run",
        .parent_runtime_key = "runtime:cli:default",
        .parent_session_id = parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:child",
        .task_summary = "Old queued work",
    });

    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    const auto before_cleanup = manager.status(SubagentStatusRequest{.run_id = "stale-run"});
    ASSERT_TRUE(before_cleanup.run.has_value());
    EXPECT_EQ(before_cleanup.run->status, SubagentRunStatus::queued);

    manager.abandon_stale_runs("runtime:cli:default");

    const auto after_cleanup = manager.status(SubagentStatusRequest{.run_id = "stale-run"});
    ASSERT_TRUE(after_cleanup.run.has_value());
    EXPECT_EQ(after_cleanup.run->status, SubagentRunStatus::abandoned);
}

TEST_F(SubagentManagerTest, ExplicitCleanupOnlyAbandonsMatchingRuntime) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    run_store.create_run(SubagentRunCreateParams{
        .run_id = "matching-runtime-run",
        .parent_runtime_key = "runtime:cli:default",
        .parent_session_id = parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:child",
        .task_summary = "Matching runtime",
    });
    run_store.create_run(SubagentRunCreateParams{
        .run_id = "other-runtime-run",
        .parent_runtime_key = "runtime:channel:other",
        .parent_session_id = parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:child",
        .task_summary = "Other runtime",
    });

    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    manager.abandon_stale_runs("runtime:cli:default");

    const auto matching = manager.status(SubagentStatusRequest{.run_id = "matching-runtime-run"});
    ASSERT_TRUE(matching.run.has_value());
    EXPECT_EQ(matching.run->status, SubagentRunStatus::abandoned);

    const auto other = manager.status(SubagentStatusRequest{.run_id = "other-runtime-run"});
    ASSERT_TRUE(other.run.has_value());
    EXPECT_EQ(other.run->status, SubagentRunStatus::queued);
}

TEST_F(SubagentManagerTest, StatusAndWaitOnlyReturnRunsToOwningRuntime) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Done",
            .final_output = "Detailed output",
        };
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(spawn_result.accepted);

    auto other_caller = sample_caller_context(parent_session_id);
    other_caller.runtime_key = "runtime:cli:other";

    const auto other_status = manager.status(SubagentStatusRequest{
        .run_id = spawn_result.run_id,
        .caller = other_caller,
    });
    EXPECT_FALSE(other_status.run.has_value());

    const auto other_wait = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
        .caller = other_caller,
    });
    EXPECT_EQ(other_wait.state, SubagentWaitState::not_found);
    EXPECT_FALSE(other_wait.run.has_value());

    const auto owner_status = manager.status(SubagentStatusRequest{
        .run_id = spawn_result.run_id,
        .caller = sample_caller_context(parent_session_id),
    });
    ASSERT_TRUE(owner_status.run.has_value());
    EXPECT_EQ(owner_status.run->run_id, spawn_result.run_id);
}

TEST_F(SubagentManagerTest, StatusPollingReapsFinishedRunsBeforeShutdown) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    std::promise<void> release_slow_worker;
    auto release_slow_worker_future = release_slow_worker.get_future().share();
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &request) {
        if (request.task_summary == "Investigate failing parser tests") {
            return SubagentWorkerResult{
                .status = SubagentRunStatus::succeeded,
                .final_summary = "Fast result",
                .final_output = "Fast output",
            };
        }
        release_slow_worker_future.wait();
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Slow result",
            .final_output = "Slow output",
        };
    });

    const auto fast_run = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(fast_run.accepted);

    SubagentStatusResult fast_status;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
        fast_status = manager.status(SubagentStatusRequest{.run_id = fast_run.run_id});
        if (fast_status.run.has_value() && fast_status.run->status == SubagentRunStatus::succeeded) {
            break;
        }
        std::this_thread::yield();
    }
    ASSERT_TRUE(fast_status.run.has_value());
    ASSERT_EQ(fast_status.run->status, SubagentRunStatus::succeeded);

    auto slow_request = sample_spawn_request(parent_session_id, child_session_id);
    slow_request.task_summary = "Slow task";
    const auto slow_run = manager.spawn(slow_request);
    ASSERT_TRUE(slow_run.accepted);

    manager.shutdown();
    release_slow_worker.set_value();

    const auto fast_after_shutdown = manager.status(SubagentStatusRequest{.run_id = fast_run.run_id});
    ASSERT_TRUE(fast_after_shutdown.run.has_value());
    EXPECT_EQ(fast_after_shutdown.run->status, SubagentRunStatus::succeeded);

    const auto slow_after_shutdown = manager.status(SubagentStatusRequest{.run_id = slow_run.run_id});
    ASSERT_TRUE(slow_after_shutdown.run.has_value());
    EXPECT_EQ(slow_after_shutdown.run->status, SubagentRunStatus::abandoned);
}

TEST_F(SubagentManagerTest, ShutdownAbandonsOnlyManagersRunsAndRequestsCooperativeCancellation) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    run_store.create_run(SubagentRunCreateParams{
        .run_id = "external-queued-run",
        .parent_runtime_key = "runtime:external",
        .parent_session_id = parent_session_id,
        .parent_agent_key = "external",
        .child_session_id = child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:child",
        .task_summary = "External queued work",
    });

    std::promise<void> worker_started;
    std::promise<void> worker_stopped;
    auto worker_started_future = worker_started.get_future();
    auto worker_stopped_future = worker_stopped.get_future();
    std::atomic<bool> stop_requested{false};
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &request) {
        worker_started.set_value();
        while (!request.stop_token.stop_requested()) {
            std::this_thread::yield();
        }
        stop_requested = true;
        worker_stopped.set_value();
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Should not persist",
            .final_output = "Should not persist",
        };
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(spawn_result.accepted);
    ASSERT_EQ(worker_started_future.wait_for(std::chrono::seconds{1}), std::future_status::ready);

    manager.shutdown();

    ASSERT_EQ(worker_stopped_future.wait_for(std::chrono::seconds{1}), std::future_status::ready);
    EXPECT_TRUE(stop_requested.load());

    const auto abandoned = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
    ASSERT_TRUE(abandoned.run.has_value());
    EXPECT_EQ(abandoned.run->status, SubagentRunStatus::abandoned);

    const auto external_run = manager.status(SubagentStatusRequest{.run_id = "external-queued-run"});
    ASSERT_TRUE(external_run.run.has_value());
    EXPECT_EQ(external_run.run->status, SubagentRunStatus::queued);

    const auto wait_result = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::seconds{1},
    });
    ASSERT_EQ(wait_result.state, SubagentWaitState::completed);
    ASSERT_TRUE(wait_result.run.has_value());
    EXPECT_EQ(wait_result.run->status, SubagentRunStatus::abandoned);
}

TEST_F(SubagentManagerTest, ShutdownUnblocksWaitersImmediatelyAfterAbandonment) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    std::promise<void> worker_started;
    std::promise<void> release_worker;
    auto worker_started_future = worker_started.get_future();
    auto release_worker_future = release_worker.get_future().share();
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &request) {
        worker_started.set_value();
        while (!request.stop_token.stop_requested()) {
            std::this_thread::yield();
        }
        release_worker_future.wait();
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(spawn_result.accepted);
    ASSERT_EQ(worker_started_future.wait_for(std::chrono::seconds{1}), std::future_status::ready);

    auto shutdown_future = std::async(std::launch::async, [&] {
        manager.shutdown();
    });

    const auto wait_result = manager.wait(SubagentWaitRequest{
        .run_id = spawn_result.run_id,
        .timeout = std::chrono::milliseconds{50},
    });

    EXPECT_EQ(wait_result.state, SubagentWaitState::completed);
    ASSERT_TRUE(wait_result.run.has_value());
    EXPECT_EQ(wait_result.run->status, SubagentRunStatus::abandoned);

    release_worker.set_value();
    shutdown_future.get();
}

TEST_F(SubagentManagerTest, ShutdownIgnoresRunsThatAlreadyReachedTerminalState) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore run_store(db_path().string());
    std::promise<void> worker_started;
    std::promise<void> allow_worker_to_finish;
    auto worker_started_future = worker_started.get_future();
    auto allow_worker_to_finish_future = allow_worker_to_finish.get_future().share();
    SubagentManager manager(run_store, [&](const SubagentWorkerRequest &) {
        worker_started.set_value();
        allow_worker_to_finish_future.wait();
        return SubagentWorkerResult{
            .status = SubagentRunStatus::succeeded,
            .final_summary = "Done",
            .final_output = "Output",
        };
    });

    const auto spawn_result = manager.spawn(sample_spawn_request(parent_session_id, child_session_id));
    ASSERT_TRUE(spawn_result.accepted);
    ASSERT_EQ(worker_started_future.wait_for(std::chrono::seconds{1}), std::future_status::ready);

    allow_worker_to_finish.set_value();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status_result = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
        if (status_result.run.has_value() && status_result.run->status == SubagentRunStatus::succeeded) {
            break;
        }
        std::this_thread::yield();
    }

    EXPECT_NO_THROW(manager.shutdown());

    const auto final_status = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
    ASSERT_TRUE(final_status.run.has_value());
    EXPECT_EQ(final_status.run->status, SubagentRunStatus::succeeded);
}
