#include "infra/storage/session-store.hpp"
#include "infra/storage/subagent-run-store.hpp"
#include "providers/provider.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>
#include <thread>
#include <unordered_map>

using namespace orangutan;

namespace {

    class ScriptedProvider final : public Provider {
    public:
        using Step = std::function<LLMResponse(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &)>;

        explicit ScriptedProvider(std::vector<Step> steps)
        : steps_(std::move(steps)) {}

        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &, int,
                                int = 0) override {
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
        std::size_t next_step_ = 0;
    };

    const ToolDef *find_tool(const std::vector<ToolDef> &tools, const std::string &name) {
        const auto it = std::ranges::find_if(tools, [&](const ToolDef &tool) {
            return tool.name == name;
        });
        return it == tools.end() ? nullptr : &*it;
    }

    template <typename Fn>
    bool completes_without_throw(Fn &&fn) {
        try {
            std::forward<Fn>(fn)();
            return true;
        } catch (...) {
            return false;
        }
    }

    struct SubagentManagerHarness {
        SubagentManagerHarness()
        : db_path(orangutan::testing::unique_test_db_path("subagent-manager", "runs.db")) {}

        ~SubagentManagerHarness() {
            std::filesystem::remove_all(db_path.parent_path());
        }

        SubagentManagerHarness(const SubagentManagerHarness &) = delete;
        SubagentManagerHarness &operator=(const SubagentManagerHarness &) = delete;
        SubagentManagerHarness(SubagentManagerHarness &&) = delete;
        SubagentManagerHarness &operator=(SubagentManagerHarness &&) = delete;

        [[nodiscard]]
        std::array<std::string, 2> create_linked_sessions() const {
            SessionStore session_store(db_path);
            return {
                session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
                session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:child", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
            };
        }

        [[nodiscard]]
        static SubagentCallerContext sample_caller_context(const std::string &parent_session_id, bool is_child_run = false) {
            return SubagentCallerContext{
                .runtime_origin = base::origin::cli,
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

        std::filesystem::path db_path;
    };

    TEST_CASE("spawn_returns_accepted_run_and_wait_returns_final_result_using_fake_worker") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);

        auto row_existed_before_worker = false;
        SubagentManager manager(run_store, [&](const SubagentWorkerRequest &request) {
            SubagentRunStore worker_store(harness.db_path);
            row_existed_before_worker = worker_store.load_run(request.run_id).has_value();
            return SubagentWorkerResult{
                .status = SubagentRunStatus::succeeded,
                .final_summary = "Done",
                .final_output = "Detailed output",
            };
        });

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));

        REQUIRE(spawn_result.accepted);
        CHECK_FALSE(spawn_result.run_id.empty());
        CHECK(spawn_result.error.empty());

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
        });

        CHECK(wait_result.state == SubagentWaitState::completed);
        REQUIRE(wait_result.run.has_value());
        CHECK(wait_result.run->status == SubagentRunStatus::succeeded);
        CHECK(wait_result.run->final_summary == "Done");
        CHECK(wait_result.run->final_output == "Detailed output");
        CHECK(row_existed_before_worker);

        const auto status_result = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
        REQUIRE(status_result.run.has_value());
        CHECK(status_result.run->status == SubagentRunStatus::succeeded);
        CHECK(status_result.run->parent_runtime_key == "runtime:cli:default");
        CHECK(status_result.run->child_agent_key == "coder");
    };

    TEST_CASE("child_callers_are_rejected") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });

        auto request = SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id);
        request.caller.is_child_run = true;

        const auto spawn_result = manager.spawn(request);

        CHECK_FALSE(spawn_result.accepted);
        CHECK(spawn_result.run_id.empty());
        CHECK_FALSE(spawn_result.error.empty());
    };

    TEST_CASE("allowlist_violations_are_rejected") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
        });

        auto request = SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id);
        request.caller.allowed_child_agents = {"reviewer"};

        const auto spawn_result = manager.spawn(request);

        CHECK_FALSE(spawn_result.accepted);
        CHECK(spawn_result.run_id.empty());
        CHECK_FALSE(spawn_result.error.empty());
    };

    TEST_CASE("wait_can_time_out_cleanly") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(spawn_result.accepted);

        const auto timed_out = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::milliseconds{10},
        });
        CHECK(timed_out.state == SubagentWaitState::timed_out);
        CHECK_FALSE(timed_out.run.has_value());

        release_worker.set_value();

        const auto completed = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
        });
        CHECK(completed.state == SubagentWaitState::completed);
        REQUIRE(completed.run.has_value());
        CHECK(completed.run->status == SubagentRunStatus::succeeded);
    };

    TEST_CASE("real_child_inherits_caller_approval_callback") {
        SubagentManagerHarness harness;
        SessionStore session_store(harness.db_path);
        SubagentRunStore run_store(harness.db_path);

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
                                           .workspace_root = orangutan::testing::unique_test_root("subagent-manager-child-approval").string(),
                                           .permissions = child_permissions,
                                       });

        bool prompted = false;
        SubagentManager manager(run_store, SubagentExecutionEnvironment{
                                               .agent_configs = &child_configs,
                                               .session_store = &session_store,
                                               .memory_store = nullptr,
                                               .provider_factory =
                                                   [&](const SubagentChildRuntimeConfig &) {
                                                       std::vector<ScriptedProvider::Step> steps;
                                                       steps.reserve(2);
                                                       steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                                                           return LLMResponse{
                                                               .stop_reason = "tool_use",
                                                               .content = {ToolUse("child-shell", "shell", nlohmann::json{{"command", "echo child"}})},
                                                           };
                                                       });
                                                       steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &) -> LLMResponse {
                                                           return LLMResponse{
                                                               .stop_reason = "end_turn",
                                                               .content = {Text{"child completed"}},
                                                           };
                                                       });
                                                       return std::make_unique<ScriptedProvider>(std::move(steps));
                                                   },
                                           });

        auto caller = SubagentManagerHarness::sample_caller_context(std::string{});
        caller.session_id = std::nullopt;
        caller.allowed_child_agents = {"coder"};
        caller.approval_callback = [&prompted](const ToolUse &call, const std::string &prompt_text) {
            prompted = true;
            CHECK(call.name == "shell");
            CHECK(prompt_text.contains("echo child"));
            return true;
        };

        const auto spawn_result = manager.spawn(SubagentSpawnRequest{
            .caller = caller,
            .child_agent_key = "coder",
            .task_summary = "Run a child shell command",
        });
        REQUIRE(spawn_result.accepted);

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
            .caller = caller,
        });
        CHECK(wait_result.state == SubagentWaitState::completed);
        REQUIRE(wait_result.run.has_value());
        CHECK(wait_result.run->status == SubagentRunStatus::succeeded);
        CHECK(prompted);
    };

    TEST_CASE("real_child_uses_configured_hashline_edit_mode") {
        SubagentManagerHarness harness;
        SessionStore session_store(harness.db_path);
        SubagentRunStore run_store(harness.db_path);

        std::unordered_map<std::string, SubagentChildRuntimeConfig> child_configs;
        child_configs.emplace("coder", SubagentChildRuntimeConfig{
                                           .agent_key = "coder",
                                           .provider_name = "child-provider",
                                           .api_key = "unused",
                                           .model = "child-model",
                                           .base_url = "https://example.test",
                                           .system_prompt = "Child base prompt.",
                                           .workspace_root = orangutan::testing::unique_test_root("subagent-manager-child-hashline").string(),
                                           .edit_mode = "hashline",
                                       });

        SubagentManager manager(run_store, SubagentExecutionEnvironment{
                                               .agent_configs = &child_configs,
                                               .session_store = &session_store,
                                               .memory_store = nullptr,
                                               .provider_factory =
                                                   [&](const SubagentChildRuntimeConfig &) {
                                                       std::vector<ScriptedProvider::Step> steps;
                                                       steps.reserve(1);
                                                       steps.emplace_back([&](std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &tools) -> LLMResponse {
                                                           const auto *edit_tool = find_tool(tools, "edit");
                                                           REQUIRE(edit_tool != nullptr);
                                                           CHECK(edit_tool->description.contains("hash"));
                                                           REQUIRE(edit_tool->input_schema.contains("properties"));
                                                           CHECK(edit_tool->input_schema["properties"].contains("edits"));
                                                           CHECK_FALSE(edit_tool->input_schema["properties"].contains("patch"));

                                                           const auto *read_tool = find_tool(tools, "read");
                                                           REQUIRE(read_tool != nullptr);
                                                           CHECK(read_tool->description.contains("line numbers"));

                                                           return LLMResponse{
                                                               .stop_reason = "end_turn",
                                                               .content = {Text{"child completed"}},
                                                           };
                                                       });
                                                       return std::make_unique<ScriptedProvider>(std::move(steps));
                                                   },
                                           });

        auto caller = SubagentManagerHarness::sample_caller_context(std::string{});
        caller.session_id = std::nullopt;
        caller.allowed_child_agents = {"coder"};

        const auto spawn_result = manager.spawn(SubagentSpawnRequest{
            .caller = caller,
            .child_agent_key = "coder",
            .task_summary = "Inspect available tools",
        });
        REQUIRE(spawn_result.accepted);

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
            .caller = caller,
        });
        CHECK(wait_result.state == SubagentWaitState::completed);
        REQUIRE(wait_result.run.has_value());
        CHECK(wait_result.run->status == SubagentRunStatus::succeeded);
    };

    TEST_CASE("constructor_does_not_abandon_stale_runs_and_explicit_cleanup_does") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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
        REQUIRE(before_cleanup.run.has_value());
        CHECK(before_cleanup.run->status == SubagentRunStatus::queued);

        manager.abandon_stale_runs("runtime:cli:default");

        const auto after_cleanup = manager.status(SubagentStatusRequest{.run_id = "stale-run"});
        REQUIRE(after_cleanup.run.has_value());
        CHECK(after_cleanup.run->status == SubagentRunStatus::abandoned);
    };

    TEST_CASE("explicit_cleanup_only_abandons_matching_runtime") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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
        REQUIRE(matching.run.has_value());
        CHECK(matching.run->status == SubagentRunStatus::abandoned);

        const auto other = manager.status(SubagentStatusRequest{.run_id = "other-runtime-run"});
        REQUIRE(other.run.has_value());
        CHECK(other.run->status == SubagentRunStatus::queued);
    };

    TEST_CASE("status_and_wait_only_return_runs_to_owning_runtime") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
        SubagentManager manager(run_store, [](const SubagentWorkerRequest &) {
            return SubagentWorkerResult{
                .status = SubagentRunStatus::succeeded,
                .final_summary = "Done",
                .final_output = "Detailed output",
            };
        });

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(spawn_result.accepted);

        auto other_caller = SubagentManagerHarness::sample_caller_context(parent_session_id);
        other_caller.runtime_key = "runtime:cli:other";

        const auto other_status = manager.status(SubagentStatusRequest{
            .run_id = spawn_result.run_id,
            .caller = other_caller,
        });
        CHECK_FALSE(other_status.run.has_value());

        const auto other_wait = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
            .caller = other_caller,
        });
        CHECK(other_wait.state == SubagentWaitState::not_found);
        CHECK_FALSE(other_wait.run.has_value());

        const auto owner_status = manager.status(SubagentStatusRequest{
            .run_id = spawn_result.run_id,
            .caller = SubagentManagerHarness::sample_caller_context(parent_session_id),
        });
        REQUIRE(owner_status.run.has_value());
        CHECK(owner_status.run->run_id == spawn_result.run_id);
    };

    TEST_CASE("status_polling_reaps_finished_runs_before_shutdown") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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

        const auto fast_run = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(fast_run.accepted);

        SubagentStatusResult fast_status;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (std::chrono::steady_clock::now() < deadline) {
            fast_status = manager.status(SubagentStatusRequest{.run_id = fast_run.run_id});
            if (fast_status.run.has_value() && fast_status.run->status == SubagentRunStatus::succeeded) {
                break;
            }
            std::this_thread::yield();
        }
        REQUIRE(fast_status.run.has_value());
        CHECK(fast_status.run->status == SubagentRunStatus::succeeded);

        auto slow_request = SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id);
        slow_request.task_summary = "Slow task";
        const auto slow_run = manager.spawn(slow_request);
        REQUIRE(slow_run.accepted);

        manager.shutdown();
        release_slow_worker.set_value();

        const auto fast_after_shutdown = manager.status(SubagentStatusRequest{.run_id = fast_run.run_id});
        REQUIRE(fast_after_shutdown.run.has_value());
        CHECK(fast_after_shutdown.run->status == SubagentRunStatus::succeeded);

        const auto slow_after_shutdown = manager.status(SubagentStatusRequest{.run_id = slow_run.run_id});
        REQUIRE(slow_after_shutdown.run.has_value());
        CHECK(slow_after_shutdown.run->status == SubagentRunStatus::abandoned);
    };

    TEST_CASE("shutdown_abandons_only_managers_runs_and_requests_cooperative_cancellation") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(spawn_result.accepted);
        CHECK(worker_started_future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

        manager.shutdown();

        CHECK(worker_stopped_future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
        CHECK(stop_requested.load());

        const auto abandoned = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
        REQUIRE(abandoned.run.has_value());
        CHECK(abandoned.run->status == SubagentRunStatus::abandoned);

        const auto external_run = manager.status(SubagentStatusRequest{.run_id = "external-queued-run"});
        REQUIRE(external_run.run.has_value());
        CHECK(external_run.run->status == SubagentRunStatus::queued);

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::seconds{1},
        });
        CHECK(wait_result.state == SubagentWaitState::completed);
        REQUIRE(wait_result.run.has_value());
        CHECK(wait_result.run->status == SubagentRunStatus::abandoned);
    };

    TEST_CASE("shutdown_unblocks_waiters_immediately_after_abandonment") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(spawn_result.accepted);
        CHECK(worker_started_future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

        auto shutdown_future = std::async(std::launch::async, [&] {
            manager.shutdown();
        });

        const auto wait_result = manager.wait(SubagentWaitRequest{
            .run_id = spawn_result.run_id,
            .timeout = std::chrono::milliseconds{50},
        });

        CHECK(wait_result.state == SubagentWaitState::completed);
        REQUIRE(wait_result.run.has_value());
        CHECK(wait_result.run->status == SubagentRunStatus::abandoned);

        release_worker.set_value();
        shutdown_future.get();
    };

    TEST_CASE("shutdown_ignores_runs_that_already_reached_terminal_state") {
        SubagentManagerHarness harness;
        const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
        SubagentRunStore run_store(harness.db_path);
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

        const auto spawn_result = manager.spawn(SubagentManagerHarness::sample_spawn_request(parent_session_id, child_session_id));
        REQUIRE(spawn_result.accepted);
        CHECK(worker_started_future.wait_for(std::chrono::seconds{1}) == std::future_status::ready);

        allow_worker_to_finish.set_value();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status_result = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
            if (status_result.run.has_value() && status_result.run->status == SubagentRunStatus::succeeded) {
                break;
            }
            std::this_thread::yield();
        }

        CHECK(completes_without_throw([&] {
            manager.shutdown();
        }));

        const auto final_status = manager.status(SubagentStatusRequest{.run_id = spawn_result.run_id});
        REQUIRE(final_status.run.has_value());
        CHECK(final_status.run->status == SubagentRunStatus::succeeded);
    };

} // namespace
