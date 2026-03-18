#include "infra/storage/subagent-run-store.hpp"
#include "infra/storage/session-store.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <cstdlib>
#include <array>
#include <sqlite3.h>

using namespace orangutan;

class SubagentRunStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = std::filesystem::temp_directory_path() / "orangutan_subagent_run_store_test.db";
        std::filesystem::remove(db_path_);
    }

    void TearDown() override {
        std::filesystem::remove(db_path_);
    }

    [[nodiscard]]
    static std::filesystem::path shared_default_db_path(const std::filesystem::path &home_dir) {
        return home_dir / ".orangutan" / "sessions.db";
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
    static SubagentRunCreateParams sample_create_params(const std::string &parent_session_id, const std::string &child_session_id) {
        return SubagentRunCreateParams{
            .run_id = "run-123",
            .parent_runtime_key = "runtime:parent",
            .parent_session_id = parent_session_id,
            .parent_agent_key = "default",
            .child_session_id = child_session_id,
            .child_agent_key = "coder",
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

TEST_F(SubagentRunStoreTest, CreateRunPersistsMetadataAndStartsQueued) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(sample_create_params(parent_session_id, child_session_id));

    const auto maybe_run = store.load_run("run-123");
    ASSERT_TRUE(maybe_run.has_value());
    EXPECT_EQ(maybe_run->run_id, "run-123");
    EXPECT_EQ(maybe_run->parent_runtime_key, "runtime:parent");
    ASSERT_TRUE(maybe_run->parent_session_id.has_value());
    EXPECT_EQ(*maybe_run->parent_session_id, parent_session_id);
    EXPECT_EQ(maybe_run->parent_agent_key, "default");
    EXPECT_EQ(maybe_run->child_session_id, child_session_id);
    EXPECT_EQ(maybe_run->child_agent_key, "coder");
    EXPECT_EQ(maybe_run->child_scope_key, "scope:child");
    EXPECT_EQ(maybe_run->status, SubagentRunStatus::queued);
    EXPECT_EQ(maybe_run->task_summary, "Investigate failing parser tests");
    EXPECT_TRUE(maybe_run->final_summary.empty());
    EXPECT_TRUE(maybe_run->final_output.empty());
    EXPECT_TRUE(maybe_run->error_text.empty());
    EXPECT_FALSE(maybe_run->created_at.empty());
    EXPECT_FALSE(maybe_run->started_at.has_value());
    EXPECT_FALSE(maybe_run->finished_at.has_value());
}

TEST_F(SubagentRunStoreTest, LoadRunReturnsNulloptForUnknownId) {
    SubagentRunStore store(db_path().string());

    EXPECT_EQ(store.load_run("missing-run"), std::nullopt);
}

TEST_F(SubagentRunStoreTest, MarkRunningUpdatesStatusAndStartedAt) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());
    store.create_run(sample_create_params(parent_session_id, child_session_id));

    store.mark_running("run-123");

    const auto maybe_run = store.load_run("run-123");
    ASSERT_TRUE(maybe_run.has_value());
    EXPECT_EQ(maybe_run->status, SubagentRunStatus::running);
    ASSERT_TRUE(maybe_run->started_at.has_value());
    EXPECT_FALSE(maybe_run->started_at->empty());
    EXPECT_FALSE(maybe_run->finished_at.has_value());
}

TEST_F(SubagentRunStoreTest, TerminalTransitionsPersistSummariesAndErrors) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(sample_create_params(parent_session_id, child_session_id));
    store.mark_running("run-123");
    store.mark_succeeded("run-123", "Done", "Detailed output");

    const auto succeeded_run = store.load_run("run-123");
    ASSERT_TRUE(succeeded_run.has_value());
    EXPECT_EQ(succeeded_run->status, SubagentRunStatus::succeeded);
    EXPECT_EQ(succeeded_run->final_summary, "Done");
    EXPECT_EQ(succeeded_run->final_output, "Detailed output");
    EXPECT_TRUE(succeeded_run->error_text.empty());
    ASSERT_TRUE(succeeded_run->finished_at.has_value());
    EXPECT_FALSE(succeeded_run->finished_at->empty());

    const auto [failed_parent_session_id, failed_child_session_id] = create_linked_sessions();
    const auto failed_params = SubagentRunCreateParams{
        .run_id = "run-failed",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = failed_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = failed_child_session_id,
        .child_agent_key = "reviewer",
        .child_scope_key = "scope:review",
        .task_summary = "Review the implementation",
    };
    store.create_run(failed_params);
    store.mark_failed("run-failed", "subagent crashed");

    const auto failed_run = store.load_run("run-failed");
    ASSERT_TRUE(failed_run.has_value());
    EXPECT_EQ(failed_run->status, SubagentRunStatus::failed);
    ASSERT_TRUE(failed_run->parent_session_id.has_value());
    EXPECT_EQ(*failed_run->parent_session_id, failed_parent_session_id);
    EXPECT_EQ(failed_run->error_text, "subagent crashed");
    ASSERT_TRUE(failed_run->finished_at.has_value());

    const auto [timed_out_parent_session_id, timed_out_child_session_id] = create_linked_sessions();
    const auto timed_out_params = SubagentRunCreateParams{
        .run_id = "run-timeout",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = timed_out_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = timed_out_child_session_id,
        .child_agent_key = "reviewer",
        .child_scope_key = "scope:review",
        .task_summary = "Wait for a reply",
    };
    store.create_run(timed_out_params);
    store.mark_timed_out("run-timeout", "timed out waiting for child");

    const auto timed_out_run = store.load_run("run-timeout");
    ASSERT_TRUE(timed_out_run.has_value());
    EXPECT_EQ(timed_out_run->status, SubagentRunStatus::timed_out);
    EXPECT_EQ(timed_out_run->error_text, "timed out waiting for child");
    ASSERT_TRUE(timed_out_run->finished_at.has_value());
}

TEST_F(SubagentRunStoreTest, MarkActiveRunsAbandonedOnlyTouchesQueuedAndRunningRows) {
    const auto [queued_parent_session_id, queued_child_session_id] = create_linked_sessions();
    const auto [running_parent_session_id, running_child_session_id] = create_linked_sessions();
    const auto [succeeded_parent_session_id, succeeded_child_session_id] = create_linked_sessions();
    const auto [failed_parent_session_id, failed_child_session_id] = create_linked_sessions();
    const auto [timeout_parent_session_id, timeout_child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(SubagentRunCreateParams{
        .run_id = "queued-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = queued_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = queued_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:queued",
        .task_summary = "Queued work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "running-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = running_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = running_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:running",
        .task_summary = "Running work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "succeeded-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = succeeded_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = succeeded_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:succeeded",
        .task_summary = "Succeeded work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "failed-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = failed_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = failed_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:failed",
        .task_summary = "Failed work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "timeout-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = timeout_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = timeout_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:timeout",
        .task_summary = "Timed out work",
    });
    store.mark_running("running-run");
    store.mark_running("succeeded-run");
    store.mark_succeeded("succeeded-run", "ok", "output");
    store.mark_failed("failed-run", "bad");
    store.mark_timed_out("timeout-run", "late");

    store.mark_active_runs_abandoned();

    const auto queued_run = store.load_run("queued-run");
    ASSERT_TRUE(queued_run.has_value());
    EXPECT_EQ(queued_run->status, SubagentRunStatus::abandoned);
    ASSERT_TRUE(queued_run->finished_at.has_value());

    const auto running_run = store.load_run("running-run");
    ASSERT_TRUE(running_run.has_value());
    EXPECT_EQ(running_run->status, SubagentRunStatus::abandoned);
    ASSERT_TRUE(running_run->finished_at.has_value());

    const auto succeeded_run = store.load_run("succeeded-run");
    ASSERT_TRUE(succeeded_run.has_value());
    EXPECT_EQ(succeeded_run->status, SubagentRunStatus::succeeded);

    const auto failed_run = store.load_run("failed-run");
    ASSERT_TRUE(failed_run.has_value());
    EXPECT_EQ(failed_run->status, SubagentRunStatus::failed);

    const auto timeout_run = store.load_run("timeout-run");
    ASSERT_TRUE(timeout_run.has_value());
    EXPECT_EQ(timeout_run->status, SubagentRunStatus::timed_out);
}

TEST_F(SubagentRunStoreTest, MarkAbandonedOnlyUpdatesRequestedQueuedOrRunningRun) {
    const auto [queued_parent_session_id, queued_child_session_id] = create_linked_sessions();
    const auto [running_parent_session_id, running_child_session_id] = create_linked_sessions();
    const auto [succeeded_parent_session_id, succeeded_child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(SubagentRunCreateParams{
        .run_id = "queued-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = queued_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = queued_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:queued",
        .task_summary = "Queued work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "running-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = running_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = running_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:running",
        .task_summary = "Running work",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "succeeded-run",
        .parent_runtime_key = "runtime:parent",
        .parent_session_id = succeeded_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = succeeded_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:succeeded",
        .task_summary = "Succeeded work",
    });
    store.mark_running("running-run");
    store.mark_running("succeeded-run");
    store.mark_succeeded("succeeded-run", "ok", "output");

    store.mark_abandoned("queued-run");
    store.mark_abandoned("running-run");

    const auto queued_run = store.load_run("queued-run");
    ASSERT_TRUE(queued_run.has_value());
    EXPECT_EQ(queued_run->status, SubagentRunStatus::abandoned);
    ASSERT_TRUE(queued_run->finished_at.has_value());

    const auto running_run = store.load_run("running-run");
    ASSERT_TRUE(running_run.has_value());
    EXPECT_EQ(running_run->status, SubagentRunStatus::abandoned);
    ASSERT_TRUE(running_run->finished_at.has_value());

    EXPECT_THROW(store.mark_abandoned("succeeded-run"), std::runtime_error);
}

TEST_F(SubagentRunStoreTest, MarkActiveRunsAbandonedForRuntimeOnlyTouchesMatchingRuntime) {
    const auto [first_parent_session_id, first_child_session_id] = create_linked_sessions();
    const auto [second_parent_session_id, second_child_session_id] = create_linked_sessions();
    const auto [third_parent_session_id, third_child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(SubagentRunCreateParams{
        .run_id = "runtime-a-queued",
        .parent_runtime_key = "runtime:a",
        .parent_session_id = first_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = first_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:a",
        .task_summary = "Queued A",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "runtime-a-running",
        .parent_runtime_key = "runtime:a",
        .parent_session_id = second_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = second_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:a",
        .task_summary = "Running A",
    });
    store.create_run(SubagentRunCreateParams{
        .run_id = "runtime-b-queued",
        .parent_runtime_key = "runtime:b",
        .parent_session_id = third_parent_session_id,
        .parent_agent_key = "default",
        .child_session_id = third_child_session_id,
        .child_agent_key = "coder",
        .child_scope_key = "scope:b",
        .task_summary = "Queued B",
    });
    store.mark_running("runtime-a-running");

    store.mark_active_runs_abandoned_for_runtime("runtime:a");

    const auto runtime_a_queued = store.load_run("runtime-a-queued");
    ASSERT_TRUE(runtime_a_queued.has_value());
    EXPECT_EQ(runtime_a_queued->status, SubagentRunStatus::abandoned);

    const auto runtime_a_running = store.load_run("runtime-a-running");
    ASSERT_TRUE(runtime_a_running.has_value());
    EXPECT_EQ(runtime_a_running->status, SubagentRunStatus::abandoned);

    const auto runtime_b_queued = store.load_run("runtime-b-queued");
    ASSERT_TRUE(runtime_b_queued.has_value());
    EXPECT_EQ(runtime_b_queued->status, SubagentRunStatus::queued);
}

TEST_F(SubagentRunStoreTest, DefaultConstructorUsesSharedSessionDatabasePath) {
    auto *const original_home = std::getenv("HOME");
    const auto temp_home = std::filesystem::temp_directory_path() / "orangutan_subagent_run_store_home";
    std::filesystem::remove_all(temp_home);
    std::filesystem::create_directories(temp_home);
    ASSERT_EQ(setenv("HOME", temp_home.string().c_str(), 1), 0);

    {
        SessionStore session_store;
        SubagentRunStore run_store;

        const auto parent_session_id = session_store.create_empty("test-model", "scope:test");
        const auto child_session_id = session_store.create_empty("test-model", "scope:test");
        run_store.create_run(SubagentRunCreateParams{
            .run_id = "shared-db-run",
            .parent_runtime_key = "runtime:shared",
            .parent_session_id = parent_session_id,
            .parent_agent_key = "default",
            .child_session_id = child_session_id,
            .child_agent_key = "coder",
            .child_scope_key = "scope:shared",
            .task_summary = "Shared DB test",
        });
    }

    sqlite3 *db = nullptr;
    const auto db_path = shared_default_db_path(temp_home);
    ASSERT_EQ(sqlite3_open(db_path.string().c_str(), &db), SQLITE_OK);

    sqlite3_stmt *stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sessions", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM subagent_runs", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (original_home != nullptr) {
        ASSERT_EQ(setenv("HOME", original_home, 1), 0);
    } else {
        ASSERT_EQ(unsetenv("HOME"), 0);
    }
    std::filesystem::remove_all(temp_home);
}

TEST_F(SubagentRunStoreTest, MarkRunningThrowsForUnknownRunId) {
    SubagentRunStore store(db_path().string());

    EXPECT_THROW(store.mark_running("missing-run"), std::runtime_error);
}

TEST_F(SubagentRunStoreTest, TerminalRunsRejectFurtherStateTransitions) {
    const auto [parent_session_id, child_session_id] = create_linked_sessions();
    SubagentRunStore store(db_path().string());

    store.create_run(sample_create_params(parent_session_id, child_session_id));
    store.mark_running("run-123");
    store.mark_succeeded("run-123", "Done", "Detailed output");

    EXPECT_THROW(store.mark_timed_out("run-123", "too late"), std::runtime_error);
    EXPECT_THROW(store.mark_failed("run-123", "too late"), std::runtime_error);
    EXPECT_THROW(store.mark_running("run-123"), std::runtime_error);

    const auto maybe_run = store.load_run("run-123");
    ASSERT_TRUE(maybe_run.has_value());
    EXPECT_EQ(maybe_run->status, SubagentRunStatus::succeeded);
    EXPECT_EQ(maybe_run->final_summary, "Done");
    EXPECT_EQ(maybe_run->final_output, "Detailed output");
}

TEST_F(SubagentRunStoreTest, CreateRunRequiresExistingSessionLinks) {
    SessionStore session_store(db_path().string());
    const auto parent_session_id = session_store.create_empty("test-model", "scope:parent");
    SubagentRunStore store(db_path().string());

    EXPECT_THROW(store.create_run(SubagentRunCreateParams{
                     .run_id = "broken-run",
                     .parent_runtime_key = "runtime:parent",
                     .parent_session_id = parent_session_id,
                     .parent_agent_key = "default",
                     .child_session_id = "missing-child-session",
                     .child_agent_key = "coder",
                     .child_scope_key = "scope:child",
                     .task_summary = "This should fail",
                 }),
                 std::runtime_error);
}
