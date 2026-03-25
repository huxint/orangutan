#include "infra/storage/subagent-run-store.hpp"
#include "infra/storage/session-store.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <array>
#include <optional>
#include <sqlite3.h>

using namespace orangutan;

namespace {

struct SubagentRunStoreHarness {
    SubagentRunStoreHarness()
    : db_path(orangutan::testing::unique_test_db_path("subagent-run-store", "runs.db")) {}

    ~SubagentRunStoreHarness() {
        std::filesystem::remove_all(db_path.parent_path());
    }

    [[nodiscard]]
    static std::filesystem::path shared_default_db_path(const std::filesystem::path &home_dir) {
        return home_dir / ".orangutan" / "sessions.db";
    }

    [[nodiscard]]
    std::array<std::string, 2> create_linked_sessions() const {
        SessionStore session_store(db_path);
        return {
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:child", .agent_key = "", .origin_kind = "cli", .origin_ref = ""}),
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

    std::filesystem::path db_path;
};

struct ScopedHomeOverride {
    explicit ScopedHomeOverride(std::filesystem::path home_dir)
    : original_home(std::getenv("HOME")),
      temp_home(std::move(home_dir)) {
        std::filesystem::create_directories(temp_home);
        CHECK((setenv("HOME", temp_home.string().c_str(), 1) == 0));
    }

    ~ScopedHomeOverride() {
        if (original_home.has_value()) {
            CHECK(setenv("HOME", original_home->c_str(), 1) == 0);
        } else {
            CHECK(unsetenv("HOME") == 0);
        }
        std::filesystem::remove_all(temp_home);
    }

    std::optional<std::string> original_home;
    std::filesystem::path temp_home;
};

TEST_CASE("create_run_persists_metadata_and_starts_queued") {
    SubagentRunStoreHarness harness;
    const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

    store.create_run(SubagentRunStoreHarness::sample_create_params(parent_session_id, child_session_id));

    const auto maybe_run = store.load_run("run-123");
    REQUIRE(maybe_run.has_value());
    CHECK(maybe_run->run_id == "run-123");
    CHECK(maybe_run->parent_runtime_key == "runtime:parent");
    REQUIRE(maybe_run->parent_session_id.has_value());
    CHECK(*maybe_run->parent_session_id == parent_session_id);
    CHECK(maybe_run->parent_agent_key == "default");
    CHECK(maybe_run->child_session_id == child_session_id);
    CHECK(maybe_run->child_agent_key == "coder");
    CHECK(maybe_run->child_scope_key == "scope:child");
    CHECK(maybe_run->status == SubagentRunStatus::queued);
    CHECK(maybe_run->task_summary == "Investigate failing parser tests");
    CHECK(maybe_run->final_summary.empty());
    CHECK(maybe_run->final_output.empty());
    CHECK(maybe_run->error_text.empty());
    CHECK_FALSE(maybe_run->created_at.empty());
    CHECK_FALSE(maybe_run->started_at.has_value());
    CHECK_FALSE(maybe_run->finished_at.has_value());
};

TEST_CASE("load_run_returns_nullopt_for_unknown_id") {
    SubagentRunStoreHarness harness;
    SubagentRunStore store(harness.db_path);

    CHECK(store.load_run("missing-run") == std::nullopt);
};

TEST_CASE("load_run_throws_for_invalid_persisted_status_text") {
    SubagentRunStoreHarness harness;
    const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

    store.create_run(SubagentRunStoreHarness::sample_create_params(parent_session_id, child_session_id));

    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(harness.db_path.string().c_str(), &db) == SQLITE_OK);
    REQUIRE(sqlite3_exec(db, "UPDATE subagent_runs SET status = 'bogus-status' WHERE run_id = 'run-123'", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);

    try {
        static_cast<void>(store.load_run("run-123"));
        FAIL();
    } catch (const std::runtime_error &error) {
        CHECK(std::string_view{error.what()} == "Unknown subagent run status: bogus-status");
    }
};

TEST_CASE("mark_running_updates_status_and_started_at") {
    SubagentRunStoreHarness harness;
    const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);
    store.create_run(SubagentRunStoreHarness::sample_create_params(parent_session_id, child_session_id));

    store.mark_running("run-123");

    const auto maybe_run = store.load_run("run-123");
    REQUIRE(maybe_run.has_value());
    CHECK(maybe_run->status == SubagentRunStatus::running);
    REQUIRE(maybe_run->started_at.has_value());
    CHECK_FALSE(maybe_run->started_at->empty());
    CHECK_FALSE(maybe_run->finished_at.has_value());
};

TEST_CASE("terminal_transitions_persist_summaries_and_errors") {
    SubagentRunStoreHarness harness;
    const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

    store.create_run(SubagentRunStoreHarness::sample_create_params(parent_session_id, child_session_id));
    store.mark_running("run-123");
    store.mark_succeeded("run-123", "Done", "Detailed output");

    const auto succeeded_run = store.load_run("run-123");
    REQUIRE(succeeded_run.has_value());
    CHECK(succeeded_run->status == SubagentRunStatus::succeeded);
    CHECK(succeeded_run->final_summary == "Done");
    CHECK(succeeded_run->final_output == "Detailed output");
    CHECK(succeeded_run->error_text.empty());
    REQUIRE(succeeded_run->finished_at.has_value());
    CHECK_FALSE(succeeded_run->finished_at->empty());

    const auto [failed_parent_session_id, failed_child_session_id] = harness.create_linked_sessions();
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
    REQUIRE(failed_run.has_value());
    CHECK(failed_run->status == SubagentRunStatus::failed);
    REQUIRE(failed_run->parent_session_id.has_value());
    CHECK(*failed_run->parent_session_id == failed_parent_session_id);
    CHECK(failed_run->error_text == "subagent crashed");
    REQUIRE(failed_run->finished_at.has_value());

    const auto [timed_out_parent_session_id, timed_out_child_session_id] = harness.create_linked_sessions();
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
    REQUIRE(timed_out_run.has_value());
    CHECK(timed_out_run->status == SubagentRunStatus::timed_out);
    CHECK(timed_out_run->error_text == "timed out waiting for child");
    REQUIRE(timed_out_run->finished_at.has_value());
};

TEST_CASE("mark_active_runs_abandoned_only_touches_queued_and_running_rows") {
    SubagentRunStoreHarness harness;
    const auto [queued_parent_session_id, queued_child_session_id] = harness.create_linked_sessions();
    const auto [running_parent_session_id, running_child_session_id] = harness.create_linked_sessions();
    const auto [succeeded_parent_session_id, succeeded_child_session_id] = harness.create_linked_sessions();
    const auto [failed_parent_session_id, failed_child_session_id] = harness.create_linked_sessions();
    const auto [timeout_parent_session_id, timeout_child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

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
    REQUIRE(queued_run.has_value());
    CHECK(queued_run->status == SubagentRunStatus::abandoned);
    REQUIRE(queued_run->finished_at.has_value());

    const auto running_run = store.load_run("running-run");
    REQUIRE(running_run.has_value());
    CHECK(running_run->status == SubagentRunStatus::abandoned);
    REQUIRE(running_run->finished_at.has_value());

    const auto succeeded_run = store.load_run("succeeded-run");
    REQUIRE(succeeded_run.has_value());
    CHECK(succeeded_run->status == SubagentRunStatus::succeeded);

    const auto failed_run = store.load_run("failed-run");
    REQUIRE(failed_run.has_value());
    CHECK(failed_run->status == SubagentRunStatus::failed);

    const auto timeout_run = store.load_run("timeout-run");
    REQUIRE(timeout_run.has_value());
    CHECK(timeout_run->status == SubagentRunStatus::timed_out);
};

TEST_CASE("mark_abandoned_only_updates_requested_queued_or_running_run") {
    SubagentRunStoreHarness harness;
    const auto [queued_parent_session_id, queued_child_session_id] = harness.create_linked_sessions();
    const auto [running_parent_session_id, running_child_session_id] = harness.create_linked_sessions();
    const auto [succeeded_parent_session_id, succeeded_child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

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
    REQUIRE(queued_run.has_value());
    CHECK(queued_run->status == SubagentRunStatus::abandoned);
    REQUIRE(queued_run->finished_at.has_value());

    const auto running_run = store.load_run("running-run");
    REQUIRE(running_run.has_value());
    CHECK(running_run->status == SubagentRunStatus::abandoned);
    REQUIRE(running_run->finished_at.has_value());

    REQUIRE_THROWS_AS(store.mark_abandoned("succeeded-run"), std::runtime_error);
};

TEST_CASE("mark_active_runs_abandoned_for_runtime_only_touches_matching_runtime") {
    SubagentRunStoreHarness harness;
    const auto [first_parent_session_id, first_child_session_id] = harness.create_linked_sessions();
    const auto [second_parent_session_id, second_child_session_id] = harness.create_linked_sessions();
    const auto [third_parent_session_id, third_child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

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
    REQUIRE(runtime_a_queued.has_value());
    CHECK(runtime_a_queued->status == SubagentRunStatus::abandoned);

    const auto runtime_a_running = store.load_run("runtime-a-running");
    REQUIRE(runtime_a_running.has_value());
    CHECK(runtime_a_running->status == SubagentRunStatus::abandoned);

    const auto runtime_b_queued = store.load_run("runtime-b-queued");
    REQUIRE(runtime_b_queued.has_value());
    CHECK(runtime_b_queued->status == SubagentRunStatus::queued);
};

TEST_CASE("default_constructor_uses_shared_session_database_path") {
    ScopedHomeOverride home_override(orangutan::testing::unique_test_root("subagent-run-store-home"));

    {
        SessionStore session_store;
        SubagentRunStore run_store;

        const auto parent_session_id =
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:test", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        const auto child_session_id =
            session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:test", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
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
    const auto db_path = SubagentRunStoreHarness::shared_default_db_path(home_override.temp_home);
    REQUIRE(sqlite3_open(db_path.string().c_str(), &db) == SQLITE_OK);

    sqlite3_stmt *stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sessions", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 2);
    sqlite3_finalize(stmt);

    REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM subagent_runs", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
};

TEST_CASE("mark_running_throws_for_unknown_run_id") {
    SubagentRunStoreHarness harness;
    SubagentRunStore store(harness.db_path);

    REQUIRE_THROWS_AS(store.mark_running("missing-run"), std::runtime_error);
};

TEST_CASE("terminal_runs_reject_further_state_transitions") {
    SubagentRunStoreHarness harness;
    const auto [parent_session_id, child_session_id] = harness.create_linked_sessions();
    SubagentRunStore store(harness.db_path);

    store.create_run(SubagentRunStoreHarness::sample_create_params(parent_session_id, child_session_id));
    store.mark_running("run-123");
    store.mark_succeeded("run-123", "Done", "Detailed output");

    REQUIRE_THROWS_AS(store.mark_timed_out("run-123", "too late"), std::runtime_error);
    REQUIRE_THROWS_AS(store.mark_failed("run-123", "too late"), std::runtime_error);
    REQUIRE_THROWS_AS(store.mark_running("run-123"), std::runtime_error);

    const auto maybe_run = store.load_run("run-123");
    REQUIRE(maybe_run.has_value());
    CHECK(maybe_run->status == SubagentRunStatus::succeeded);
    CHECK(maybe_run->final_summary == "Done");
    CHECK(maybe_run->final_output == "Detailed output");
};

TEST_CASE("create_run_requires_existing_session_links") {
    SubagentRunStoreHarness harness;
    SessionStore session_store(harness.db_path);
    const auto parent_session_id =
        session_store.create_empty(orangutan::SessionMetadata{.model = "test-model", .scope_key = "scope:parent", .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
    SubagentRunStore store(harness.db_path);

    REQUIRE_THROWS_AS(store.create_run(SubagentRunCreateParams{
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
};

} // namespace