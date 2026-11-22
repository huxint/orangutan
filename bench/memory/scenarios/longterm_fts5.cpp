#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async.hpp>
#include <oran/core/time.hpp>
#include <oran/memory.hpp>
#include <oran/storage.hpp>

namespace orangutan::bench {
namespace {

constexpr std::size_t kCorpusSize = 10'000;
constexpr std::size_t kSearchLimit = 10;
constexpr std::string_view kScopeKey = "agent:bench";

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

[[nodiscard]] memory::longterm::RecordKind kind_for(std::size_t index) noexcept {
  switch (index % 4U) {
    case 0:
      return memory::longterm::RecordKind::project;
    case 1:
      return memory::longterm::RecordKind::reference;
    case 2:
      return memory::longterm::RecordKind::user;
    default:
      return memory::longterm::RecordKind::feedback;
  }
}

[[nodiscard]] memory::longterm::Record make_record(std::size_t index) {
  const auto created = core::Time{core::Time::time_point{std::chrono::seconds{1}}};
  const auto updated = core::Time{core::Time::time_point{std::chrono::seconds{2}}};
  return memory::longterm::Record{
      .key =
          memory::longterm::RecordKey{
              .id = std::format("rec-{:05}", index),
              .scope_key = std::string{kScopeKey},
          },
      .kind = kind_for(index),
      .title = std::format("React agent loop note {}", index),
      .body = std::format("Synthetic long-term memory row {} for the react agent loop search baseline. "
                          "The record mentions scoped tools, recall ranking, and prompt-boundary memory.",
                          index),
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = static_cast<double>(index % 100U) / 100.0,
      .tags = {"react", "agent", "loop", std::format("bucket-{}", index % 16U)},
      .linked_record_ids = {},
  };
}

void seed_corpus(asio::io_context& io, memory::longterm::Fts5Backend& backend) {
  bool seeded = false;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto migrated = co_await backend.migrate();
        if (!migrated) {
          std::abort();
        }
        for (std::size_t i = 0; i < kCorpusSize; ++i) {
          auto stored = co_await backend.upsert(make_record(i));
          if (!stored) {
            std::abort();
          }
        }
        seeded = true;
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (!seeded) {
    std::abort();
  }
}

[[gnu::noinline]] std::size_t run_fts5_search(asio::io_context& io, memory::longterm::Backend& backend) {
  std::size_t rows = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto hits = co_await backend.search(
            memory::longterm::Query{
                .scope_key = std::string{kScopeKey},
                .text = "react agent loop",
                .kinds = {},
            },
            kSearchLimit);
        if (!hits) {
          std::abort();
        }
        rows = hits->size();
        co_return;
      },
      asio::detached);
  io.restart();
  io.run();
  if (rows != kSearchLimit) {
    std::abort();
  }
  return rows;
}

}  // namespace

void register_longterm_fts5(ankerl::nanobench::Bench& bench) {
  TempDb db{"memory-longterm-fts5"};

  asio::io_context io;
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.path(), .reader_count = 2, .statement_cache_capacity = 32});
  if (!pool) {
    std::abort();
  }
  memory::longterm::Fts5Backend backend{*pool};
  seed_corpus(io, backend);

  bench.epochs(5).minEpochIterations(20).warmup(2);
  bench.run("memory.longterm_fts5_search_10k_limit10", [&] {
    const auto rows = run_fts5_search(io, backend);
    ankerl::nanobench::doNotOptimizeAway(rows);
  });
}

}  // namespace orangutan::bench
