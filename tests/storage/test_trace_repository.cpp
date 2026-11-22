// tests/storage/test_trace_repository.cpp — trace_turns repository coverage.

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

storage::TraceId id_with(unsigned char seed) {
  storage::TraceId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

storage::AppendTraceTurnRequest
make_request(storage::TraceId turn_id, storage::TraceId session_id, std::int64_t started_at_ns) {
  return storage::AppendTraceTurnRequest{
      .turn_id = turn_id,
      .session_id = session_id,
      .agent_key = "coder",
      .origin = "cli",
      .route_profile = "fake-main",
      .route_model = "fake-model",
      .started_at_ns = started_at_ns,
      .finished_at_ns = started_at_ns + 25,
      .stop_reason = "end_turn",
      .iteration_count = 1,
      .prompt_prefix_hash = 0xfeed'face'1234'5678ULL,
      .prompt_prefix_bytes = 1024,
      .active_catalog_hash = 0x1111'2222'3333'4444ULL,
      .cache_creation_tokens = 2,
      .cache_read_tokens = 3,
      .input_tokens = 1500,
      .output_tokens = 200,
      .cost_estimate_usd = 0.012,
      .context_json = R"json({"source":"test"})json",
  };
}

}  // namespace

TEST_CASE("TraceRepository::migrate applies the trace schema once", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-migrate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{pool};

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

TEST_CASE("TraceRepository::migrate upgrades an existing audit schema", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-upgrade-audit"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    {
      auto writer = co_await pool.acquire_writer();
      REQUIRE(writer.has_value());
      auto report = storage::run_migrations(writer->connection(), storage::built_in_audit_migrations().first(1));
      REQUIRE(report.has_value());
      REQUIRE(report->current_version == 1);
      REQUIRE(report->applied_versions == std::vector<std::int64_t>{1});
    }

    storage::TraceRepository repo{pool};
    auto upgraded = co_await repo.migrate();
    REQUIRE(upgraded.has_value());
    REQUIRE(upgraded->previous_version == 1);
    REQUIRE(upgraded->current_version == 6);
    REQUIRE(upgraded->applied_versions == std::vector<std::int64_t>{2, 3, 4, 5, 6});

    auto request = make_request(id_with(0x10), id_with(0x80), 1'000);
    auto appended = co_await repo.append_turn(std::move(request));
    REQUIRE(appended.has_value());
    REQUIRE(appended->turn_id == id_with(0x10));
  });
}

TEST_CASE("TraceRepository::migrate accepts an explicit migration directory", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-migrate-dir"};
  TempDir migrations{"oran-trace-repo-migrations"};
  write_file(migrations.path() / "0001-custom-marker.sql", "CREATE TABLE custom_trace_marker(id INTEGER PRIMARY KEY)");

  test::run_async([&db, &migrations](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{
        pool,
        storage::TraceRepositoryOptions{.migrations_directory = migrations.string()},
    };

    auto report = co_await repo.migrate();
    REQUIRE(report.has_value());
    REQUIRE(report->current_version == 1);

    auto reader = co_await pool.acquire_reader();
    REQUIRE(reader.has_value());
    auto marker = reader->connection().query(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'custom_trace_marker'");
    REQUIRE(marker.has_value());
    REQUIRE(marker->rows.size() == 1);
  });
}

TEST_CASE("TraceRepository append_turn round-trips a redacted turn row", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-roundtrip"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto parent_id = id_with(0x20);
    auto request = make_request(id_with(0x10), id_with(0x80), 1'000);
    request.parent_turn_id = parent_id;
    request.stop_reason = "cancelled";
    request.cancellation_phase = "provider";

    auto appended = co_await repo.append_turn(request);
    REQUIRE(appended.has_value());
    REQUIRE(appended->turn_id == id_with(0x10));
    REQUIRE(appended->parent_turn_id.has_value());
    REQUIRE(*appended->parent_turn_id == parent_id);
    REQUIRE(appended->session_id == id_with(0x80));
    REQUIRE(appended->agent_key == "coder");
    REQUIRE(appended->origin == "cli");
    REQUIRE(appended->route_profile == "fake-main");
    REQUIRE(appended->route_model == "fake-model");
    REQUIRE(appended->started_at_ns == 1'000);
    REQUIRE(appended->finished_at_ns == 1'025);
    REQUIRE(appended->stop_reason == "cancelled");
    REQUIRE(appended->iteration_count == 1);
    REQUIRE(appended->prompt_prefix_hash == 0xfeed'face'1234'5678ULL);
    REQUIRE(appended->prompt_prefix_bytes == 1024);
    REQUIRE(appended->active_catalog_hash == 0x1111'2222'3333'4444ULL);
    REQUIRE(appended->cache_creation_tokens == 2);
    REQUIRE(appended->cache_read_tokens == 3);
    REQUIRE(appended->input_tokens == 1500);
    REQUIRE(appended->output_tokens == 200);
    REQUIRE(appended->cost_estimate_usd == 0.012);
    REQUIRE(appended->cancellation_phase == "provider");
    REQUIRE(appended->context_json == R"json({"source":"test"})json");
    REQUIRE(appended->schema_version == 1);

    auto loaded = co_await repo.get_turn(id_with(0x10));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->has_value());
    REQUIRE((*loaded)->turn_id == id_with(0x10));
    REQUIRE((*loaded)->context_json == R"json({"source":"test"})json");
  });
}

TEST_CASE("TraceRepository list_turns orders newest first and applies filters", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-list"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto session_a = id_with(0x40);
    auto session_b = id_with(0x60);
    auto a1 = co_await repo.append_turn(make_request(id_with(0x01), session_a, 100));
    REQUIRE(a1.has_value());
    auto a2 = co_await repo.append_turn(make_request(id_with(0x02), session_a, 300));
    REQUIRE(a2.has_value());
    auto b1_request = make_request(id_with(0x03), session_b, 200);
    b1_request.agent_key = "reviewer";
    auto b1 = co_await repo.append_turn(std::move(b1_request));
    REQUIRE(b1.has_value());

    auto all = co_await repo.list_turns(storage::ListTraceTurnsOptions{.limit = 10});
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 3);
    REQUIRE((*all)[0].turn_id == id_with(0x02));
    REQUIRE((*all)[1].turn_id == id_with(0x03));
    REQUIRE((*all)[2].turn_id == id_with(0x01));

    auto only_session_a =
        co_await repo.list_turns(storage::ListTraceTurnsOptions{.session_id = session_a, .limit = 10});
    REQUIRE(only_session_a.has_value());
    REQUIRE(only_session_a->size() == 2);
    REQUIRE((*only_session_a)[0].turn_id == id_with(0x02));
    REQUIRE((*only_session_a)[1].turn_id == id_with(0x01));

    auto only_reviewer = co_await repo.list_turns(storage::ListTraceTurnsOptions{.agent_key = "reviewer", .limit = 10});
    REQUIRE(only_reviewer.has_value());
    REQUIRE(only_reviewer->size() == 1);
    REQUIRE((*only_reviewer)[0].turn_id == id_with(0x03));

    auto limited = co_await repo.list_turns(storage::ListTraceTurnsOptions{.limit = 1});
    REQUIRE(limited.has_value());
    REQUIRE(limited->size() == 1);
    REQUIRE((*limited)[0].turn_id == id_with(0x02));
  });
}

TEST_CASE("TraceRepository returns nullopt for missing turns", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-missing"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{pool};
    auto migrated = co_await repo.migrate();
    REQUIRE(migrated.has_value());

    auto loaded = co_await repo.get_turn(id_with(0x10));
    REQUIRE(loaded.has_value());
    REQUIRE_FALSE(loaded->has_value());
  });
}

TEST_CASE("TraceRepository validates required fields before SQLite", "[unit][storage][trace_repository]") {
  TempDb db{"oran-trace-repo-validate"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_pool(io, db);
    storage::TraceRepository repo{pool};

    auto missing_turn = make_request(storage::TraceId{}, id_with(0x20), 100);
    auto missing_turn_result = co_await repo.append_turn(std::move(missing_turn));
    REQUIRE_FALSE(missing_turn_result.has_value());
    REQUIRE(missing_turn_result.error().kind() == core::ErrorKind::invalid_argument);

    auto missing_agent = make_request(id_with(0x10), id_with(0x20), 100);
    missing_agent.agent_key.clear();
    auto missing_agent_result = co_await repo.append_turn(std::move(missing_agent));
    REQUIRE_FALSE(missing_agent_result.has_value());
    REQUIRE(missing_agent_result.error().kind() == core::ErrorKind::invalid_argument);

    auto backwards_time = make_request(id_with(0x11), id_with(0x20), 100);
    backwards_time.finished_at_ns = 99;
    auto backwards_time_result = co_await repo.append_turn(std::move(backwards_time));
    REQUIRE_FALSE(backwards_time_result.has_value());
    REQUIRE(backwards_time_result.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_limit = co_await repo.list_turns(storage::ListTraceTurnsOptions{.limit = 0});
    REQUIRE_FALSE(zero_limit.has_value());
    REQUIRE(zero_limit.error().kind() == core::ErrorKind::invalid_argument);

    auto zero_get = co_await repo.get_turn(storage::TraceId{});
    REQUIRE_FALSE(zero_get.has_value());
    REQUIRE(zero_get.error().kind() == core::ErrorKind::invalid_argument);
  });
}
