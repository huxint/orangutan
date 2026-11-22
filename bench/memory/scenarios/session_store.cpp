// bench/memory/scenarios/session_store.cpp
//
// A-vs-B comparison: raw SessionRepository JSON bytes vs. typed
// memory::session::Store message serialization for one 64-message batch.

#include <nanobench.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/message.hpp>
#include <oran/memory.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {
namespace {

constexpr int kRows = 64;
constexpr std::string_view kAgentKey = "bench-agent";

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

void migrate(asio::io_context& io, storage::SessionRepository& repo) {
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

[[gnu::noinline]] int
run_repository_append_load(asio::io_context& io, storage::SessionRepository& repo, std::int64_t batch_id) {
  int rows = 0;
  const auto session_id = std::format("repo-{}", batch_id);
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kRows; ++i) {
          auto appended = co_await repo.append_message(storage::AppendSessionMessageRequest{
              .session_id = session_id,
              .agent_key = std::string{kAgentKey},
              .role = i % 2 == 0 ? core::Role::user : core::Role::assistant,
              .content_json =
                  std::format(R"json({{"version":1,"blocks":[{{"type":"text","text":"repo-{}"}}]}})json", i),
          });
          if (!appended) {
            std::abort();
          }
        }
        auto loaded = co_await repo.load_messages(
            storage::SessionKey{.session_id = session_id, .agent_key = std::string{kAgentKey}});
        if (!loaded) {
          std::abort();
        }
        rows = static_cast<int>(loaded->size());
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kRows) {
    std::abort();
  }
  return rows;
}

[[gnu::noinline]] int
run_store_append_load(asio::io_context& io, memory::session::Store& store, std::int64_t batch_id) {
  int rows = 0;
  const auto session_id = std::format("store-{}", batch_id);
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kRows; ++i) {
          auto appended = co_await store.append(memory::session::SessionId{.value = session_id},
                                                memory::session::AgentKey{.value = std::string{kAgentKey}},
                                                i % 2 == 0 ? core::Message::user_text(std::format("store-{}", i))
                                                           : core::Message::assistant_text(std::format("store-{}", i)));
          if (!appended) {
            std::abort();
          }
        }
        auto loaded = co_await store.load(memory::session::SessionId{.value = session_id},
                                          memory::session::AgentKey{.value = std::string{kAgentKey}});
        if (!loaded) {
          std::abort();
        }
        rows = static_cast<int>(loaded->size());
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kRows) {
    std::abort();
  }
  return rows;
}

}  // namespace

void register_session_store(ankerl::nanobench::Bench& bench) {
  TempDb db{"memory-session-store"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 16});
  if (!pool) {
    std::abort();
  }
  storage::SessionRepository repo{*pool};
  migrate(io, repo);
  memory::session::Store store{repo};

  std::int64_t repo_batch = 0;
  std::int64_t store_batch = 0;
  bench.run("memory.session_repository_append_load", [&] {
    auto rows = run_repository_append_load(io, repo, repo_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
  bench.run("memory.session_store_append_load", [&] {
    auto rows = run_store_append_load(io, store, store_batch++);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
