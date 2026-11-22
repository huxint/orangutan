// tests/storage/test_session_repository.cpp — sessions domain repository coverage.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
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

}  // namespace

TEST_CASE("SessionRepository::migrate applies the sessions schema once", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};

    auto first = co_await repo.migrate();
    REQUIRE(first.has_value());
    REQUIRE(first->previous_version == 0);
    REQUIRE(first->current_version == 2);
    REQUIRE(first->applied_versions == std::vector<std::int64_t>{1, 2});

    auto second = co_await repo.migrate();
    REQUIRE(second.has_value());
    REQUIRE(second->previous_version == 2);
    REQUIRE(second->current_version == 2);
    REQUIRE(second->applied_versions.empty());
  });
}

TEST_CASE("SessionRepository::migrate accepts an explicit migration directory", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-migrate-dir"};
  TempDir migrations{"oran-session-repo-migrations"};
  write_file(migrations.path() / "0001-custom-marker.sql",
             "CREATE TABLE custom_sessions_marker(id INTEGER PRIMARY KEY)");

  test::run_async([&db, &migrations](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{
        pool,
        storage::SessionRepositoryOptions{.migrations_directory = migrations.string()},
    };

    auto report = co_await repo.migrate();
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);
    REQUIRE(report->applied_versions == std::vector<std::int64_t>{1});

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto marker = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'custom_sessions_marker'");
    REQUIRE(marker.has_value());
    REQUIRE(marker->rows.size() == 1);
  });
}

TEST_CASE("SessionRepository reopening preserves saved session data while appending",
          "[unit][storage][session_repository][preservation]") {
  TempDb db{"oran-session-repo-preservation"};
  {
    auto connection = storage::Connection::open({.path = db.string()});
    REQUIRE(connection.has_value());
    auto migrated = storage::run_migrations(*connection, storage::built_in_session_migrations());
    REQUIRE(migrated.has_value());
    auto seeded = connection->execute(R"sql(
      INSERT INTO session_messages VALUES
        ('s-kept', 'coder', 1, 'user', '{"text":"saved"}', '{"source":"archive"}', '2000-01-01T00:00:00Z');
      UPDATE sessions SET title = 'Saved session', metadata_json = '{"pinned":true}';
      INSERT INTO session_skill_activations VALUES
        ('s-kept', 'coder', 'release-note', 0, '2000-01-01T00:00:00Z', '2001-01-01T00:00:00Z'),
        ('s-other', 'researcher', 'review-pr', 1, '2000-01-01T00:00:00Z', '2000-01-01T00:00:00Z');
    )sql");
    REQUIRE(seeded.has_value());
  }

  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    REQUIRE(migrated->previous_version == 2);
    REQUIRE(migrated->current_version == 2);
    REQUIRE(migrated->applied_versions.empty());
    const auto key = storage::SessionKey{.session_id = "s-kept", .agent_key = "coder"};
    auto suffix = std::vector<storage::SessionMessageInput>{
        {.role = core::Role::assistant, .content_json = R"({"text":"continued"})"}};

    auto appended = co_await repo.append_messages(key, std::move(suffix));

    REQUIRE(appended.has_value());
    REQUIRE(appended->size() == 1);
    REQUIRE(appended->front().sequence == 2);
    auto tail = co_await repo.load_tail(key);
    REQUIRE(tail.has_value());
    REQUIRE(tail->size() == 2);
    REQUIRE((*tail)[0].content_json == R"({"text":"saved"})");
    REQUIRE((*tail)[0].metadata_json == R"({"source":"archive"})");
    REQUIRE((*tail)[0].created_at == "2000-01-01T00:00:00Z");
    REQUIRE((*tail)[1].content_json == R"({"text":"continued"})");
    auto session = co_await repo.get_session(key);
    REQUIRE(session.has_value());
    REQUIRE(session->has_value());
    REQUIRE((*session)->title == "Saved session");
    REQUIRE((*session)->metadata_json == R"({"pinned":true})");
    REQUIRE((*session)->created_at == "2000-01-01T00:00:00Z");
    REQUIRE((*session)->updated_at == appended->front().created_at);
    REQUIRE((*session)->message_count == 2);

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto skills = reader->connection().query("SELECT session_id, agent_key, skill_name, active, created_at, updated_at "
                                             "FROM session_skill_activations ORDER BY session_id");
    REQUIRE(skills.has_value());
    REQUIRE(skills->rows.size() == 2);
    REQUIRE(skills->rows[0].values == std::vector<storage::ColumnValue>{"s-kept",
                                                                        "coder",
                                                                        "release-note",
                                                                        "0",
                                                                        "2000-01-01T00:00:00Z",
                                                                        "2001-01-01T00:00:00Z"});
    REQUIRE(skills->rows[1].values == std::vector<storage::ColumnValue>{"s-other",
                                                                        "researcher",
                                                                        "review-pr",
                                                                        "1",
                                                                        "2000-01-01T00:00:00Z",
                                                                        "2000-01-01T00:00:00Z"});
  });
}

TEST_CASE("SessionRepository append_message and load_messages round-trip ordered rows",
          "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto first = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-1",
        .agent_key = "coder",
        .role = core::Role::user,
        .content_json = R"json({"text":"hello"})json",
    });
    REQUIRE(first.has_value());
    REQUIRE(first->sequence == 1);
    REQUIRE(first->metadata_json == "{}");

    auto second = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "s-1",
        .agent_key = "coder",
        .role = core::Role::assistant,
        .content_json = R"json({"text":"hi"})json",
        .metadata_json = R"json({"source":"test"})json",
    });
    REQUIRE(second.has_value());
    REQUIRE(second->sequence == 2);

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-1", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].sequence == 1);
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[0].content_json == R"json({"text":"hello"})json");
    REQUIRE((*loaded)[1].sequence == 2);
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].metadata_json == R"json({"source":"test"})json");

    auto session = co_await repo.get_session(storage::SessionKey{.session_id = "s-1", .agent_key = "coder"});
    REQUIRE(session.has_value());
    REQUIRE(session->has_value());
    REQUIRE((*session)->session_id == "s-1");
    REQUIRE((*session)->agent_key == "coder");
    REQUIRE((*session)->message_count == 2);
    REQUIRE((*session)->metadata_json == "{}");
  });
}

TEST_CASE("SessionRepository round-trips every core::Role enumerator", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-role-enum"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    const std::array<core::Role, 4> roles{core::Role::user,
                                          core::Role::assistant,
                                          core::Role::system,
                                          core::Role::tool};
    for (auto role : roles) {
      auto appended = co_await repo.append_message(storage::AppendSessionMessageRequest{
          .session_id = "s-roles",
          .agent_key = "coder",
          .role = role,
          .content_json = R"json({"text":"x"})json",
      });
      REQUIRE(appended.has_value());
      REQUIRE(appended->role == role);
    }

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-roles", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == roles.size());
    for (std::size_t i = 0; i < roles.size(); ++i) {
      REQUIRE((*loaded)[i].role == roles[i]);
    }
  });
}

TEST_CASE("SessionRepository surfaces a storage error for rows with unknown role text",
          "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-bad-role"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      // Bypass the repository to inject a row whose role text is outside
      // `core::Role`. The repository must reject it on read rather than
      // silently coerce.
      auto inserted = writer->connection().execute(
          R"sql(
INSERT INTO session_messages(session_id, agent_key, sequence, role, content_json, metadata_json, created_at)
VALUES ('s-bad', 'coder', 1, 'sidekick', '{}', '{}', '2026-05-16T00:00:00.000Z')
)sql");
      REQUIRE(inserted.has_value());
    }

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "s-bad", .agent_key = "coder"});
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().kind() == core::ErrorKind::storage);
  });
}

TEST_CASE("SessionRepository returns empty results for missing sessions", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-missing"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto loaded = co_await repo.load_messages(storage::SessionKey{.session_id = "missing", .agent_key = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->empty());

    auto session = co_await repo.get_session(storage::SessionKey{.session_id = "missing", .agent_key = "coder"});
    REQUIRE(session.has_value());
    REQUIRE_FALSE(session->has_value());
  });
}

TEST_CASE("SessionRepository validates required fields", "[unit][storage][session_repository]") {
  TempDb db{"oran-session-repo-invalid"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};

    auto append = co_await repo.append_message(storage::AppendSessionMessageRequest{
        .session_id = "",
        .agent_key = "coder",
        .role = core::Role::user,
        .content_json = "{}",
    });
    REQUIRE_FALSE(append.has_value());
    REQUIRE(append.error().kind() == core::ErrorKind::invalid_argument);

    auto load = co_await repo.load_messages(storage::SessionKey{.session_id = "s-1", .agent_key = ""});
    REQUIRE_FALSE(load.has_value());
    REQUIRE(load.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("SessionRepository appends complete suffixes in order", "[unit][storage][session_repository][atomic]") {
  TempDb db{"oran-session-suffix-order"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    const auto key = storage::SessionKey{.session_id = "s-1", .agent_key = "coder"};
    auto messages = std::vector<storage::SessionMessageInput>{
        {.role = core::Role::user, .content_json = R"({"text":"question"})"},
        {.role = core::Role::assistant,
         .content_json = R"({"text":"answer"})",
         .metadata_json = R"({"source":"model"})"},
    };

    auto appended = co_await repo.append_messages(key, std::move(messages));
    REQUIRE(appended.has_value());
    REQUIRE(appended->size() == 2);
    REQUIRE((*appended)[0].sequence == 1);
    REQUIRE((*appended)[1].sequence == 2);
    auto next =
        co_await repo.append_message(storage::AppendSessionMessageRequest{.session_id = "s-1",
                                                                          .agent_key = "coder",
                                                                          .content_json = R"({"text":"next"})"});
    REQUIRE(next.has_value());
    REQUIRE(next->sequence == 3);

    auto loaded = co_await repo.load_messages(key);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 3);
    REQUIRE((*loaded)[0].content_json == R"({"text":"question"})");
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[1].content_json == R"({"text":"answer"})");
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].metadata_json == R"({"source":"model"})");
    REQUIRE((*loaded)[2].content_json == R"({"text":"next"})");
    auto session = co_await repo.get_session(key);
    REQUIRE(session.has_value());
    REQUIRE(session->has_value());
    REQUIRE((*session)->message_count == 3);
  });
}

TEST_CASE("SessionRepository rolls back the suffix and session row on a later insert failure",
          "[unit][storage][session_repository][atomic]") {
  bool existing_session = false;
  SECTION("a new session") {}
  SECTION("an existing conversation") {
    existing_session = true;
  }
  TempDb db{"oran-session-suffix-rollback"};
  test::run_async([&db, existing_session](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    const auto key = storage::SessionKey{.session_id = "s-1", .agent_key = "coder"};
    if (existing_session) {
      auto prior =
          co_await repo.append_message(storage::AppendSessionMessageRequest{.session_id = "s-1",
                                                                            .agent_key = "coder",
                                                                            .content_json = R"({"text":"saved"})"});
      REQUIRE(prior.has_value());
    }
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      auto setup = writer->connection().execute(R"sql(
        UPDATE sessions SET updated_at = '2000-01-01T00:00:00Z';
        CREATE TRIGGER reject_suffix BEFORE INSERT ON session_messages
        WHEN NEW.content_json = 'reject'
        BEGIN SELECT RAISE(ABORT, 'reject later message'); END;
      )sql");
      REQUIRE(setup.has_value());
    }
    auto messages = std::vector<storage::SessionMessageInput>{
        {.content_json = R"({"text":"pending"})"},
        {.role = core::Role::assistant, .content_json = "reject"},
    };

    auto appended = co_await repo.append_messages(key, std::move(messages));

    REQUIRE_FALSE(appended.has_value());
    REQUIRE(appended.error().kind() == core::ErrorKind::storage);
    REQUIRE(std::ranges::contains(appended.error().context(),
                                  core::Error::ContextEntry{"sqlite_message", "reject later message"}));
    auto loaded = co_await repo.load_messages(key);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == (existing_session ? 1 : 0));
    auto session = co_await repo.get_session(key);
    REQUIRE(session.has_value());
    REQUIRE(session->has_value() == existing_session);
    if (existing_session) {
      REQUIRE((*loaded)[0].content_json == R"({"text":"saved"})");
      REQUIRE((*session)->updated_at == "2000-01-01T00:00:00Z");
    }
    auto retry =
        co_await repo.append_message(storage::AppendSessionMessageRequest{.session_id = "s-1",
                                                                          .agent_key = "coder",
                                                                          .content_json = R"({"text":"retry"})"});
    REQUIRE(retry.has_value());
    REQUIRE(retry->sequence == (existing_session ? 2 : 1));
  });
}

TEST_CASE("SessionRepository isolates suffixes by both session and agent",
          "[unit][storage][session_repository][atomic]") {
  TempDb db{"oran-session-suffix-scope"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());
    const auto keys = std::array{
        storage::SessionKey{.session_id = "s-1", .agent_key = "coder"},
        storage::SessionKey{.session_id = "s-1", .agent_key = "researcher"},
        storage::SessionKey{.session_id = "s-2", .agent_key = "coder"},
    };
    for (const auto& key : keys) {
      auto messages =
          std::vector<storage::SessionMessageInput>{{.content_json = key.session_id}, {.content_json = key.agent_key}};
      auto appended = co_await repo.append_messages(key, std::move(messages));
      REQUIRE(appended.has_value());
    }

    for (const auto& key : keys) {
      auto loaded = co_await repo.load_messages(key);
      REQUIRE(loaded.has_value());
      REQUIRE(loaded->size() == 2);
      REQUIRE((*loaded)[0].sequence == 1);
      REQUIRE((*loaded)[0].content_json == key.session_id);
      REQUIRE((*loaded)[1].sequence == 2);
      REQUIRE((*loaded)[1].content_json == key.agent_key);
    }
  });
}

TEST_CASE("SessionRepository leaves storage untouched for empty or invalid suffixes",
          "[unit][storage][session_repository][atomic]") {
  TempDb db{"oran-session-suffix-validation"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    const auto key = storage::SessionKey{.session_id = "s-1", .agent_key = "coder"};
    SECTION("empty suffix needs no schema") {
      auto appended = co_await repo.append_messages(key, {});
      REQUIRE(appended.has_value());
      REQUIRE(appended->empty());
    }
    SECTION("invalid later message fails before accessing the schema") {
      auto messages = std::vector<storage::SessionMessageInput>{{.content_json = "{}"}, {.content_json = ""}};
      auto appended = co_await repo.append_messages(key, std::move(messages));
      REQUIRE_FALSE(appended.has_value());
      REQUIRE(appended.error().kind() == core::ErrorKind::invalid_argument);
      REQUIRE(std::ranges::contains(appended.error().context(), core::Error::ContextEntry{"index", "1"}));
    }
    SECTION("empty keys are invalid even for an empty suffix") {
      auto appended = co_await repo.append_messages(storage::SessionKey{}, {});
      REQUIRE_FALSE(appended.has_value());
      REQUIRE(appended.error().kind() == core::ErrorKind::invalid_argument);
    }
  });
}

TEST_CASE("Session checkpoints commit with suffixes and reject stale writers", "[storage][context]") {
  TempDb db{"oran-context-commit"};
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::SessionRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated);
    storage::SessionKey key{"session", "agent"};
    std::vector<storage::SessionMessageInput> seed{{.content_json = "first"}};
    auto seeded = co_await repo.append_messages(key, seed);
    REQUIRE(seeded);
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer);
      auto metadata = writer->connection().execute("UPDATE sessions SET metadata_json='{\"pinned\":true}'");
      REQUIRE(metadata);
    }
    storage::SessionCommit checkpoint{.expected_messages = 1,
                                      .expected_revision = 0,
                                      .checkpoint_json = R"({"covered_sequence":2,"revision":1,"summary":"handoff"})"};
    auto committed = co_await repo.append_messages(key, seed, checkpoint);
    REQUIRE(committed);
    auto session = co_await repo.get_session(key);
    REQUIRE(session);
    REQUIRE(*session);
    REQUIRE((**session).message_count == 2);
    REQUIRE((**session).metadata_json.contains("\"pinned\":true"));
    REQUIRE((**session).metadata_json.contains("handoff"));
    auto stale = co_await repo.append_messages(key, seed, checkpoint);
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().kind() == core::ErrorKind::conflict);
    checkpoint.expected_messages = 2;
    checkpoint.expected_revision = 1;
    checkpoint.checkpoint_json = R"({"covered_sequence":999,"revision":2,"summary":"bad"})";
    auto invalid = co_await repo.append_messages(key, seed, checkpoint);
    REQUIRE_FALSE(invalid);
    auto unchanged = co_await repo.get_session(key);
    REQUIRE(unchanged);
    REQUIRE((**unchanged).message_count == 2);
    REQUIRE((**unchanged).metadata_json == (**session).metadata_json);
    auto first = co_await repo.load_after(key, 0, 2, 1, 32);
    REQUIRE(first);
    REQUIRE(first->size() == 1);
    REQUIRE(first->front().sequence == 1);
    auto second = co_await repo.load_after(key, 1, 2, 1, 32);
    REQUIRE(second);
    REQUIRE(second->front().sequence == 2);
    auto oversized = co_await repo.load_after(key, 0, 2, 1, 1);
    REQUIRE_FALSE(oversized);
  });
}

TEST_CASE("Session backup includes WAL rows and import preserves explicit identities", "[storage][context][backup]") {
  TempDb db{"oran-context-source"};
  TempDb backup{"oran-context-backup"};
  TempDb imported{"oran-context-import"};
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto source_pool = open_pool(io, db);
    storage::SessionRepository source{source_pool};
    auto migrated = co_await source.migrate();
    REQUIRE(migrated);
    storage::SessionKey source_key{"original", "coder"};
    std::vector<storage::SessionMessageInput> messages{
        {.content_json = "saved", .metadata_json = R"({"origin":"user"})"}};
    auto saved = co_await source.append_messages(source_key, messages);
    REQUIRE(saved);
    {
      auto writer = co_await source_pool.acquire_writer();
      REQUIRE(writer);
      auto seeded = writer->connection().execute(R"sql(
UPDATE sessions SET title='Keep title',metadata_json='{"pinned":true,"oran_working_context":{"covered_sequence":1,"revision":1,"summary":"saved summary"}}';
INSERT INTO session_skill_activations VALUES('original','coder','build',1,'created','updated');
)sql");
      REQUIRE(seeded);
    }
    auto copied = co_await source.backup_to(backup.string());
    REQUIRE(copied);
    auto overwrite = co_await source.backup_to(backup.string());
    REQUIRE_FALSE(overwrite);
    auto later = co_await source.append_messages(source_key, messages);
    REQUIRE(later);
    auto target_pool = open_pool(io, imported);
    storage::SessionRepository target{target_pool};
    auto target_migrated = co_await target.migrate();
    REQUIRE(target_migrated);
    storage::SessionKey target_key{"restored", "reviewer"};
    auto restored = co_await target.import_session(backup.string(), source_key, target_key);
    REQUIRE(restored);
    auto rows = co_await target.load_messages(target_key);
    REQUIRE(rows);
    REQUIRE(rows->size() == 1);
    REQUIRE(rows->front().session_id == "restored");
    REQUIRE(rows->front().agent_key == "reviewer");
    REQUIRE(rows->front().content_json == "saved");
    REQUIRE(rows->front().metadata_json == messages[0].metadata_json);
    auto metadata = co_await target.get_session(target_key);
    REQUIRE(metadata);
    REQUIRE((**metadata).title == "Keep title");
    REQUIRE((**metadata).metadata_json.contains("saved summary"));
    auto duplicate = co_await target.import_session(backup.string(), source_key, target_key);
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().kind() == core::ErrorKind::conflict);
    storage::SessionKey absent{"absent", "coder"};
    storage::SessionKey empty_destination{"empty", "coder"};
    auto missing = co_await target.import_session(backup.string(), absent, empty_destination);
    REQUIRE_FALSE(missing);
    auto no_partial = co_await target.get_session(empty_destination);
    REQUIRE(no_partial);
    REQUIRE_FALSE(*no_partial);
    auto reader = co_await target_pool.acquire_reader();
    REQUIRE(reader);
    auto skills = reader->connection().query("SELECT session_id,agent_key,skill_name FROM session_skill_activations");
    REQUIRE(skills);
    REQUIRE(skills->rows.size() == 1);
    REQUIRE(skills->rows[0].values[0] == "restored");
    REQUIRE(skills->rows[0].values[1] == "reviewer");
  });
}
