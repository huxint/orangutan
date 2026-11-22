// Compare null, recording and SQLite audit sinks on one executor.
// The storage case includes coroutine dispatch and a WAL commit.

#include <nanobench.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/permission.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {

namespace {

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

permission::AuditEvent make_event(std::size_t i) {
  permission::AuditEvent event;
  event.scope_key = "scope-bench";
  event.agent_key = "bench-agent";
  event.tool_name = "FileRead";
  event.identity = "bench-identity";
  event.verdict = permission::Verdict::allow;
  event.outcome = permission::AuditOutcome::allow;
  event.reason = "rule #1 (allow: File*)";
  std::array<std::byte, 32> hash{};
  for (std::size_t j = 0; j < hash.size(); ++j) {
    hash[j] = std::byte{static_cast<std::uint8_t>((i + j) & 0xFF)};
  }
  event.input_hash = hash;
  return event;
}

void drain(asio::io_context& io) {
  io.restart();
  io.run();
}

template <typename Sink>
[[gnu::noinline]] int run_record(asio::io_context& io, Sink& sink, std::size_t i) {
  int hit = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto event = make_event(i);
        auto r = co_await sink.record(std::move(event));
        if (!r) {
          std::abort();
        }
        hit = 1;
        co_return;
      },
      asio::detached);
  drain(io);
  if (hit != 1) {
    std::abort();
  }
  return hit;
}

}  // namespace

void register_audit_scenarios(ankerl::nanobench::Bench& bench) {
  asio::io_context io;
  permission::NullAuditSink null_sink;
  permission::RecordingAuditSink recording_sink;

  TempDb db{"permission-audit"};
  auto pool_result =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 16});
  if (!pool_result) {
    std::abort();
  }
  auto pool = std::move(*pool_result);
  storage::AuditRepository repo{pool};
  {
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
    drain(io);
    if (!migrated) {
      std::abort();
    }
  }
  permission::StorageAuditSink storage_sink{repo, io.get_executor()};

  std::size_t i = 0;
  bench.run("permission.audit_null_sink", [&] {
    auto rc = run_record(io, null_sink, i++);
    ankerl::nanobench::doNotOptimizeAway(rc);
  });
  bench.run("permission.audit_recording_sink", [&] {
    auto rc = run_record(io, recording_sink, i++);
    ankerl::nanobench::doNotOptimizeAway(rc);
  });
  bench.run("permission.audit_storage_sink", [&] {
    auto rc = run_record(io, storage_sink, i++);
    ankerl::nanobench::doNotOptimizeAway(rc);
  });
  bench.run("permission.audit_to_hex_32_bytes", [&i] {
    std::array<std::byte, 32> hash{};
    for (std::size_t j = 0; j < hash.size(); ++j) {
      hash[j] = std::byte{static_cast<std::uint8_t>((i + j) & 0xFF)};
    }
    auto encoded = permission::to_hex(hash);
    ankerl::nanobench::doNotOptimizeAway(encoded);
    ++i;
  });
}

}  // namespace orangutan::bench
