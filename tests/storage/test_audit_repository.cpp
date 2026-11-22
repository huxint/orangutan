// tests/storage/test_audit_repository.cpp — audit domain repository coverage.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/storage.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;

namespace {

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
    REQUIRE_FALSE(ec);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Pool open_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  REQUIRE_FALSE(ec);

  std::ofstream output{path, std::ios::binary};
  REQUIRE(output.is_open());
  output << contents;
  REQUIRE(output.good());
}

storage::AppendAuditEventRequest make_request(std::string scope_key, std::string tool_name, std::string outcome) {
  return storage::AppendAuditEventRequest{
      .scope_key = std::move(scope_key),
      .agent_key = "coder",
      .tool_name = std::move(tool_name),
      .identity = "operator-1",
      .verdict = "allow",
      .outcome = std::move(outcome),
      .reason = "rule #1 (allow: File*)",
  };
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

}  // namespace

TEST_CASE("AuditRepository::migrate applies the audit schema once", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};

    auto first = co_await repo.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 6);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2, 3, 4, 5, 6});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 6);
    REQUIRE(second->current_version == 6);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("AuditRepository::migrate accepts an explicit migration directory", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-migrate-dir"};
  TempDir migrations{"oran-audit-repo-migrations"};
  write_file(migrations.path() / "0001-custom-marker.sql", "CREATE TABLE custom_audit_marker(id INTEGER PRIMARY KEY)");

  test::run_async([&db, &migrations](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{
        pool,
        storage::AuditRepositoryOptions{.migrations_directory = migrations.string()},
    };

    auto report = co_await repo.migrate();
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);
    REQUIRE(report->applied_versions == std::vector<std::int64_t>{1});

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto marker = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'custom_audit_marker'");
    REQUIRE(marker.has_value());
    REQUIRE(marker->rows.size() == 1);
  });
}

TEST_CASE("AuditRepository upgrade preserves saved audits and traces",
          "[unit][storage][audit_repository][preservation]") {
  TempDb db{"oran-audit-repo-preservation"};
  {
    auto connection = storage::Connection::open({.path = db.string()});
    REQUIRE(connection.has_value());
    auto migrated = storage::run_migrations(*connection, storage::built_in_audit_migrations().first(5));
    REQUIRE(migrated.has_value());
    auto seeded = connection->execute(R"sql(
      INSERT INTO trace_turns VALUES (
        X'101112131415161718191A1B1C1D1E1F', NULL, X'808182838485868788898A8B8C8D8E8F',
        'coder', 'cli', 'provider-main', 'model-a', 100, 125, 'cancelled', 2,
        42, 1024, 43, 44, 5, 7, 30, 10, 0.5, 'tools', CAST('{"source":"saved"}' AS BLOB), 1
      );
      INSERT INTO audit_events (
        id, scope_key, agent_key, tool_name, identity, verdict, outcome, reason,
        input_hash_hex, metadata_json, created_at, parent_turn_id, event_kind
      ) VALUES (
        17, 'scope-A', 'coder', 'FileWrite', 'saved-operator', 'ask', 'approved', 'saved approval',
        'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
        '{"usage":{"wall_time_ms":5.5}}', '2000-01-01T00:00:00Z',
        X'101112131415161718191A1B1C1D1E1F', 'permission_decision'
      ), (
        16, 'scope-A', 'coder', 'FileWrite', 'saved-operator', 'allow', 'allow', 'proceed',
        NULL, '{"event":"tool_before"}', '2000-01-01T00:00:00Z',
        X'101112131415161718191A1B1C1D1E1F', 'hook_publish'
      );
    )sql");
    REQUIRE(seeded.has_value());
  }

  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository audits{pool};
    storage::TraceRepository traces{pool};
    auto migrated = co_await audits.migrate();
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->previous_version == 5);
    REQUIRE(migrated->current_version == 6);
    REQUIRE(migrated->applied_versions == std::vector<std::int64_t>{6});
    auto trace_migrated = co_await traces.migrate();
    REQUIRE(trace_migrated.has_value());
    REQUIRE(trace_migrated->applied_versions.empty());

    auto appended = co_await audits.append_event(make_request("scope-A", "FileRead", "allow"));
    REQUIRE(appended.has_value());
    REQUIRE(appended->id == 18);
    auto traced = co_await traces.append_turn({.turn_id = turn_id_with(0x20),
                                              .session_id = turn_id_with(0x80),
                                              .agent_key = "coder",
                                              .origin = "cli",
                                              .route_profile = "provider-main",
                                              .route_model = "model-a",
                                              .started_at_ns = 200,
                                              .finished_at_ns = 225,
                                              .stop_reason = "end_turn"});
    REQUIRE(traced.has_value());

    auto events = co_await audits.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 3);
    REQUIRE((*events)[0].id == 18);
    const auto& saved = (*events)[1];
    REQUIRE(saved.id == 17);
    REQUIRE(saved.event_kind == "permission_decision");
    REQUIRE(saved.parent_turn_id == turn_id_with(0x10));
    REQUIRE(saved.identity == "saved-operator");
    REQUIRE(saved.verdict == "ask");
    REQUIRE(saved.outcome == "approved");
    REQUIRE(saved.reason == "saved approval");
    REQUIRE(saved.input_hash_hex == std::string(64, 'a'));
    REQUIRE(saved.metadata_json == R"({"usage":{"wall_time_ms":5.5}})");
    REQUIRE(saved.created_at == "2000-01-01T00:00:00Z");
    REQUIRE((*events)[2].id == 16);
    REQUIRE((*events)[2].event_kind == "hook_publish");
    REQUIRE((*events)[2].metadata_json == R"({"event":"tool_before"})");
    auto turn = co_await traces.get_turn(turn_id_with(0x10));
    REQUIRE(turn.has_value());
    REQUIRE(turn->has_value());
    REQUIRE((*turn)->session_id == turn_id_with(0x80));
    REQUIRE((*turn)->started_at_ns == 100);
    REQUIRE((*turn)->finished_at_ns == 125);
    REQUIRE((*turn)->stop_reason == "cancelled");
    REQUIRE((*turn)->input_tokens == 30);
    REQUIRE((*turn)->output_tokens == 10);
    REQUIRE((*turn)->cancellation_phase == "tools");
    REQUIRE((*turn)->context_json == R"({"source":"saved"})");
    auto turns = co_await traces.list_turns({});
    REQUIRE(turns.has_value());
    REQUIRE(turns->size() == 2);

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto view = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE name IN ('audit_tool_call_rollups', 'idx_audit_events_kind_parent_turn')");
    REQUIRE(view.has_value());
    REQUIRE(view->rows.empty());
  });
}

TEST_CASE("AuditRepository append_event round-trips a typical decision row", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "FileRead", "allow");
    request.input_hash_hex = std::string(64, 'a');
    request.metadata_json = R"json({"source":"test"})json";
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->id > 0);
    REQUIRE(appended->event_kind == "permission_decision");
    REQUIRE(appended->scope_key == "scope-A");
    REQUIRE(appended->tool_name == "FileRead");
    REQUIRE(appended->outcome == "allow");
    REQUIRE(appended->reason == "rule #1 (allow: File*)");
    REQUIRE(appended->input_hash_hex.has_value());
    REQUIRE(*appended->input_hash_hex == std::string(64, 'a'));
    REQUIRE_FALSE(appended->parent_turn_id.has_value());
    REQUIRE(appended->metadata_json == R"json({"source":"test"})json");
    REQUIRE_FALSE(appended->created_at.empty());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].id == appended->id);
    REQUIRE((*listed)[0].event_kind == "permission_decision");
    REQUIRE((*listed)[0].tool_name == "FileRead");
    REQUIRE_FALSE((*listed)[0].parent_turn_id.has_value());
  });
}

TEST_CASE("AuditRepository round-trips parent_turn_id blobs", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-parent-turn"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto request = make_request("scope-A", "FileRead", "allow");
    request.input_hash_hex = std::string(64, 'c');
    request.parent_turn_id = turn_id_with(0x10);
    auto appended = co_await repo.append_event(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->parent_turn_id.has_value());
    REQUIRE(*appended->parent_turn_id == turn_id_with(0x10));

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE((*listed)[0].parent_turn_id.has_value());
    REQUIRE(*(*listed)[0].parent_turn_id == turn_id_with(0x10));
  });
}

TEST_CASE("AuditRepository stores a null input_hash when the caller omits it", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-null-hash"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto appended = co_await repo.append_event(make_request("scope-A", "FileRead", "deny"));
    REQUIRE(appended.has_value());
    REQUIRE_FALSE(appended->input_hash_hex.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    REQUIRE_FALSE((*listed)[0].input_hash_hex.has_value());
  });
}

TEST_CASE("AuditRepository list_events orders newest first and applies filters", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-filters"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto a1 = co_await repo.append_event(make_request("scope-A", "FileRead", "allow"));
    REQUIRE(a1.has_value());
    auto a2 = co_await repo.append_event(make_request("scope-A", "FileWrite", "deny"));
    REQUIRE(a2.has_value());
    auto a3 = co_await repo.append_event(make_request("scope-A", "ShellExec", "approved"));
    REQUIRE(a3.has_value());
    auto a4 = co_await repo.append_event(make_request("scope-B", "FileRead", "allow"));
    REQUIRE(a4.has_value());
    auto lag = make_request("scope-A", "FileRead", "allow");
    lag.event_kind = "cancellation_lag";
    lag.metadata_json = R"json({"per_call_timeout_ms":50})json";
    auto a5 = co_await repo.append_event(std::move(lag));
    REQUIRE(a5.has_value());

    auto all = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 4);
    REQUIRE((*all)[0].event_kind == "cancellation_lag");
    REQUIRE((*all)[1].tool_name == "ShellExec");
    REQUIRE((*all)[2].tool_name == "FileWrite");
    REQUIRE((*all)[3].tool_name == "FileRead");

    auto only_deny =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .outcome = "deny"});
    REQUIRE(only_deny.has_value());
    REQUIRE(only_deny->size() == 1);
    REQUIRE((*only_deny)[0].tool_name == "FileWrite");

    auto only_file_read =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .tool_name = "FileRead"});
    REQUIRE(only_file_read.has_value());
    REQUIRE(only_file_read->size() == 2);

    auto only_lag = co_await repo.list_events(
        storage::ListAuditEventsOptions{.scope_key = "scope-A", .event_kind = "cancellation_lag"});
    REQUIRE(only_lag.has_value());
    REQUIRE(only_lag->size() == 1);
    REQUIRE((*only_lag)[0].id == a5->id);
    REQUIRE((*only_lag)[0].metadata_json == R"json({"per_call_timeout_ms":50})json");

    auto limited = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 1});
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 1);
    REQUIRE((*limited)[0].event_kind == "cancellation_lag");

    auto scope_b = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-B"});
    REQUIRE(scope_b.has_value());
    REQUIRE(scope_b->size() == 1);
    REQUIRE((*scope_b)[0].id == a4->id);
  });
}

TEST_CASE("AuditRepository validates required fields", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-validate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};

    auto missing_scope = co_await repo.append_event(storage::AppendAuditEventRequest{
        .scope_key = "",
        .agent_key = "coder",
        .tool_name = "FileRead",
        .identity = "op",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule",
    });
    REQUIRE_FALSE(missing_scope.has_value());
    REQUIRE(missing_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_event_kind = make_request("scope-A", "FileRead", "allow");
    missing_event_kind.event_kind = "";
    auto missing_event_kind_result = co_await repo.append_event(std::move(missing_event_kind));
    REQUIRE_FALSE(missing_event_kind_result.has_value());
    REQUIRE(missing_event_kind_result.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_metadata = co_await repo.append_event(storage::AppendAuditEventRequest{
        .scope_key = "scope-A",
        .agent_key = "coder",
        .tool_name = "FileRead",
        .identity = "op",
        .verdict = "allow",
        .outcome = "allow",
        .reason = "rule",
        .metadata_json = "",
    });
    REQUIRE_FALSE(missing_metadata.has_value());
    REQUIRE(missing_metadata.error().kind() == core::ErrorKind::invalid_argument);

    auto list_no_scope = co_await repo.list_events(storage::ListAuditEventsOptions{});
    REQUIRE_FALSE(list_no_scope.has_value());
    REQUIRE(list_no_scope.error().kind() == core::ErrorKind::invalid_argument);

    auto list_zero_limit =
        co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 0});
    REQUIRE_FALSE(list_zero_limit.has_value());
    REQUIRE(list_zero_limit.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_parent = make_request("scope-A", "FileRead", "allow");
    zero_parent.parent_turn_id = core::TurnId{};
    auto zero_parent_result = co_await repo.append_event(std::move(zero_parent));
    REQUIRE_FALSE(zero_parent_result.has_value());
    REQUIRE(zero_parent_result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("AuditRepository surfaces a storage error for rows with null required fields",
          "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-bad-row"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      // Drop the NOT NULL invariant on a required field, inject a row
      // that violates the application-level invariant, then restore the
      // shape. We exercise the read-side defensive parsing, not the
      // schema's own constraints.
      auto rebuild = writer->connection().execute(R"sql(
BEGIN;
ALTER TABLE audit_events RENAME TO audit_events_strict;
CREATE TABLE audit_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  event_kind TEXT NOT NULL DEFAULT 'permission_decision',
  scope_key TEXT NOT NULL,
  agent_key TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  identity TEXT NOT NULL,
  verdict TEXT NOT NULL,
  outcome TEXT NOT NULL,
  reason TEXT,
  input_hash_hex TEXT,
  parent_turn_id BLOB,
  metadata_json TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL
);
COMMIT;
)sql");
      REQUIRE(rebuild.has_value());
      auto inserted = writer->connection().execute(
          R"sql(
INSERT INTO audit_events(scope_key, agent_key, tool_name, identity, verdict, outcome, reason, metadata_json, created_at)
VALUES ('scope-A', 'coder', 'FileRead', 'op', 'allow', 'allow', NULL, '{}', '2026-05-17T00:00:00.000Z')
)sql");
      REQUIRE(inserted.has_value());
    }

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A"});
    REQUIRE_FALSE(listed.has_value());
    REQUIRE(listed.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("AuditRepository returns empty results for missing scopes", "[unit][storage][audit_repository]") {
  TempDb db{"oran-audit-repo-empty"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::AuditRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto listed = co_await repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-empty"});
    REQUIRE(listed.has_value());
    REQUIRE(listed->empty());
  });
}
