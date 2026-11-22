// bench/storage/scenarios/trace_turn_insert.cpp
//
// Per-insert latency for trace_turns, per spec 0018 AC12 ("≤ 50 µs per insert").
// A-vs-B comparison: raw Pool + StatementCache single insert vs.
// TraceRepository::append_turn single insert. Each nanobench iteration performs
// exactly one insert so the reported "ns per batch" reads directly as the
// per-turn observability cost.
//
// The sibling `trace_repository.cpp` scenario covers a 32-row batch to capture
// the amortized cost across a session-sized run; this scenario isolates the
// single-row floor that the spec acceptance criterion calls out.

#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

constexpr std::string_view kRawAppendSql = R"sql(
INSERT INTO trace_turns(
  turn_id, parent_turn_id, session_id, agent_key, origin, route_profile,
  route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count,
  prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
  deferred_catalog_hash, cache_creation_tokens, cache_read_tokens,
  input_tokens, output_tokens, cost_estimate_usd, cancellation_phase,
  context_json, schema_version
)
VALUES (?, NULL, ?, 'bench-agent', 'cli', 'fake-main', 'fake-model',
  ?, ?, 'end_turn', 1, 4096, 1024, 8192, 0, 2, 3, 1500, 200, 0.012,
  NULL, X'7b7d', 1)
)sql";

std::string make_temp_path(std::string_view tag) {
  auto path = std::filesystem::temp_directory_path() /
              (std::string{"oran-bench-"} + std::string{tag} + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  return path.string();
}

class TempDb {
public:
  explicit TempDb(std::string_view tag) : path_{make_temp_path(tag)} {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_ + "-wal", ec);
    std::filesystem::remove(path_ + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const noexcept {
    return path_;
  }

private:
  std::string path_;
};

storage::TraceId turn_id_for(std::uint64_t iter, unsigned char namespace_tag) {
  storage::TraceId id{};
  // First 8 bytes carry the namespace tag so the raw and repository scenarios
  // never collide when sharing fixture infrastructure.
  for (std::size_t i = 0; i < 8; ++i) {
    id[i] = static_cast<std::byte>(namespace_tag + static_cast<unsigned char>(i));
  }
  // Last 8 bytes carry the iteration counter so each insert sees a fresh
  // primary key even across thousands of nanobench iterations.
  std::memcpy(id.data() + 8, &iter, sizeof(iter));
  return id;
}

storage::TraceId fixed_session_id(unsigned char namespace_tag) {
  storage::TraceId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(namespace_tag ^ static_cast<unsigned char>(i));
  }
  return id;
}

void migrate(asio::io_context& io, storage::TraceRepository& repo) {
  bool migrated = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto report = co_await repo.migrate();
        if (!report) {
          std::abort();
        }
        migrated = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (!migrated) {
    std::abort();
  }
}

[[gnu::noinline]] bool run_raw_pool_single_insert(asio::io_context& io, storage::Pool& pool, std::uint64_t iter) {
  bool ok = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto writer = co_await pool.acquire_writer();
        if (!writer) {
          std::abort();
        }
        auto cached = writer->statement_cache().acquire(writer->connection(), kRawAppendSql);
        if (!cached) {
          std::abort();
        }
        const auto turn_id = turn_id_for(iter, 0x10);
        const auto session_id = fixed_session_id(0x80);
        const auto started = static_cast<std::int64_t>(iter);
        if (!cached->statement().bind_blob(1, std::span<const std::byte>{turn_id}) ||
            !cached->statement().bind_blob(2, std::span<const std::byte>{session_id}) ||
            !cached->statement().bind_int64(3, started) || !cached->statement().bind_int64(4, started + 10)) {
          std::abort();
        }
        auto step = cached->statement().step();
        if (!step || *step != storage::StepResult::done) {
          std::abort();
        }
        ok = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return ok;
}

[[gnu::noinline]] bool
run_repository_single_insert(asio::io_context& io, storage::TraceRepository& repo, std::uint64_t iter) {
  bool ok = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        const auto started = static_cast<std::int64_t>(iter);
        auto appended = co_await repo.append_turn(storage::AppendTraceTurnRequest{
            .turn_id = turn_id_for(iter, 0x20),
            .session_id = fixed_session_id(0x90),
            .agent_key = "bench-agent",
            .origin = "cli",
            .route_profile = "fake-main",
            .route_model = "fake-model",
            .started_at_ns = started,
            .finished_at_ns = started + 10,
            .stop_reason = "end_turn",
            .iteration_count = 1,
            .prompt_prefix_hash = 4096,
            .prompt_prefix_bytes = 1024,
            .active_catalog_hash = 8192,
            .cache_creation_tokens = 2,
            .cache_read_tokens = 3,
            .input_tokens = 1500,
            .output_tokens = 200,
            .cost_estimate_usd = 0.012,
        });
        if (!appended) {
          std::abort();
        }
        ok = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  return ok;
}

}  // namespace

void register_trace_turn_insert(ankerl::nanobench::Bench& bench) {
  TempDb raw_db{"trace-turn-insert-raw"};
  TempDb repo_db{"trace-turn-insert-repo"};

  asio::io_context raw_io;
  auto raw_pool = storage::Pool::open(
      raw_io.get_executor(),
      storage::PoolOptions{.path = raw_db.path(), .reader_count = 1, .statement_cache_capacity = 16});
  if (!raw_pool) {
    std::abort();
  }
  storage::TraceRepository raw_migrator{*raw_pool};
  migrate(raw_io, raw_migrator);

  asio::io_context repo_io;
  auto repo_pool = storage::Pool::open(
      repo_io.get_executor(),
      storage::PoolOptions{.path = repo_db.path(), .reader_count = 1, .statement_cache_capacity = 16});
  if (!repo_pool) {
    std::abort();
  }
  storage::TraceRepository repo{*repo_pool};
  migrate(repo_io, repo);

  std::uint64_t raw_iter = 0;
  std::uint64_t repo_iter = 0;
  bench.run("storage.trace_turn_insert_raw_pool", [&] {
    auto ok = run_raw_pool_single_insert(raw_io, *raw_pool, raw_iter++);
    ankerl::nanobench::doNotOptimizeAway(ok);
  });
  bench.run("storage.trace_turn_insert_repository", [&] {
    auto ok = run_repository_single_insert(repo_io, repo, repo_iter++);
    ankerl::nanobench::doNotOptimizeAway(ok);
  });
}

}  // namespace orangutan::bench
