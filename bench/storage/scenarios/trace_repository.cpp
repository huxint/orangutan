// bench/storage/scenarios/trace_repository.cpp
//
// A-vs-B comparison: raw Pool + StatementCache SQL vs. TraceRepository for
// inserting one batch of trace_turns rows. Spec 0018 calls out this insert as
// the per-turn observability cost, so keep the benchmark narrow and stable.

#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
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

constexpr int kRows = 32;

constexpr std::string_view kRawAppendSql = R"sql(
INSERT INTO trace_turns(
  turn_id, parent_turn_id, session_id, agent_key, origin, route_profile,
  route_model, started_at_ns, finished_at_ns, stop_reason, iteration_count,
  prompt_prefix_hash, prompt_prefix_bytes, active_catalog_hash,
  deferred_catalog_hash, cache_creation_tokens, cache_read_tokens,
  input_tokens, output_tokens, cost_estimate_usd, cancellation_phase,
  context_json, schema_version
)
VALUES (?, NULL, ?, ?, 'cli', 'fake-main', 'fake-model', ?, ?, 'end_turn', 1,
  ?, 1024, ?, 0, 2, 3, 1500, 200, 0.012, NULL, X'7b7d', 1)
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

storage::TraceId id_for(std::uint64_t batch, int row, unsigned char salt) {
  storage::TraceId id{};
  // Pack (salt, row, batch) into non-overlapping byte ranges so every
  // (batch, row, salt) tuple maps to a distinct primary key. The earlier
  // overlapping-sum encoding collided across nanobench iterations -- e.g.
  // (batch=0, row=1) and (batch=1, row=0) produced identical bytes, which
  // tripped the trace_turns PRIMARY KEY guard on the second epoch.
  id[0] = static_cast<std::byte>(salt);
  const auto row_u = static_cast<std::uint32_t>(row);
  for (std::size_t i = 0; i < 4; ++i) {
    id[1 + i] = static_cast<std::byte>((row_u >> (i * 8)) & 0xFFU);
  }
  std::memcpy(id.data() + 8, &batch, sizeof(batch));
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

[[gnu::noinline]] int run_raw_pool_insert(asio::io_context& io, storage::Pool& pool, std::uint64_t batch_id) {
  int rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        {
          auto writer = co_await pool.acquire_writer();
          if (!writer) {
            std::abort();
          }
          for (int i = 0; i < kRows; ++i) {
            auto cached = writer->statement_cache().acquire(writer->connection(), kRawAppendSql);
            if (!cached) {
              std::abort();
            }
            const auto turn_id = id_for(batch_id, i, 0x10);
            const auto session_id = id_for(batch_id, 0, 0x80);
            const auto started = static_cast<std::int64_t>((batch_id * 1'000) + i);
            if (!cached->statement().bind_blob(1, std::span<const std::byte>{turn_id}) ||
                !cached->statement().bind_blob(2, std::span<const std::byte>{session_id}) ||
                !cached->statement().bind_text(3, std::format("bench-agent-{}", batch_id)) ||
                !cached->statement().bind_int64(4, started) || !cached->statement().bind_int64(5, started + 10) ||
                !cached->statement().bind_int64(6, static_cast<std::int64_t>(0x1000U + batch_id)) ||
                !cached->statement().bind_int64(7, static_cast<std::int64_t>(0x2000U + batch_id))) {
              std::abort();
            }
            auto step = cached->statement().step();
            if (!step || *step != storage::StepResult::done) {
              std::abort();
            }
            ++rows;
          }
        }
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows <= 0) {
    std::abort();
  }
  return rows;
}

[[gnu::noinline]] int
run_repository_insert(asio::io_context& io, storage::TraceRepository& repo, std::uint64_t batch_id) {
  int rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kRows; ++i) {
          const auto started = static_cast<std::int64_t>((batch_id * 1'000) + i);
          auto appended = co_await repo.append_turn(storage::AppendTraceTurnRequest{
              .turn_id = id_for(batch_id, i, 0x20),
              .session_id = id_for(batch_id, 0, 0x90),
              .agent_key = std::format("bench-agent-{}", batch_id),
              .origin = "cli",
              .route_profile = "fake-main",
              .route_model = "fake-model",
              .started_at_ns = started,
              .finished_at_ns = started + 10,
              .stop_reason = "end_turn",
              .iteration_count = 1,
              .prompt_prefix_hash = 0x1000U + batch_id,
              .prompt_prefix_bytes = 1024,
              .active_catalog_hash = 0x2000U + batch_id,
              .cache_creation_tokens = 2,
              .cache_read_tokens = 3,
              .input_tokens = 1500,
              .output_tokens = 200,
              .cost_estimate_usd = 0.012,
          });
          if (!appended) {
            std::abort();
          }
          ++rows;
        }
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows <= 0) {
    std::abort();
  }
  return rows;
}

}  // namespace

void register_trace_repository(ankerl::nanobench::Bench& bench) {
  TempDb db{"trace-repository"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 16});
  if (!pool) {
    std::abort();
  }
  storage::TraceRepository repo{*pool};
  migrate(io, repo);

  std::uint64_t raw_batch = 0;
  std::uint64_t repo_batch = 0;
  bench.run("storage.trace_raw_pool_insert", [&] {
    auto rows = run_raw_pool_insert(io, *pool, raw_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("storage.trace_repository_insert", [&] {
    auto rows = run_repository_insert(io, repo, repo_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
