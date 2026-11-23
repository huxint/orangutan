#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/turn_id.hpp>
#include <oran/storage/pool.hpp>

#include "../../apps/personal/reminders.hpp"
#include "../test-helpers/run_async.hpp"

namespace core = orangutan::core;
namespace personal = orangutan::personal;
namespace storage = orangutan::storage;
namespace tests = orangutan::tests;

TEST_CASE("ReminderStore persists, lists, claims due, completes and deletes reminders", "[unit][personal][reminders]") {
  auto id = core::generate_turn_id();
  REQUIRE(id.has_value());
  const auto db_path =
      std::filesystem::temp_directory_path() / ("oran-reminders-" + core::format_turn_id_hex(*id) + ".db");
  struct Cleanup {
    std::filesystem::path p;
    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove(p, ec);
      std::filesystem::remove(p.string() + "-wal", ec);
      std::filesystem::remove(p.string() + "-shm", ec);
    }
  } cleanup{db_path};

  tests::run_async([&](asio::io_context& io) -> orangutan::async::Awaitable<void> {
    auto pool = storage::Pool::open(io.get_executor(), storage::PoolOptions{.path = db_path.string()});
    REQUIRE(pool.has_value());

    personal::ReminderStore store{*pool};
    REQUIRE((co_await store.migrate()).has_value());

    auto r1 = co_await store.add(personal::Reminder{
        .id = "rem-1",
        .title = "Review weekly notes",
        .due_at_ms = 1000,
        .created_at_ms = 500,
    });
    REQUIRE(r1.has_value());

    auto r2 = co_await store.add(personal::Reminder{
        .id = "rem-2",
        .title = "Future meeting",
        .due_at_ms = 5000,
        .created_at_ms = 600,
    });
    REQUIRE(r2.has_value());

    auto listed = co_await store.list();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);

    auto due = co_await store.claim_due(2000);
    REQUIRE(due.has_value());
    REQUIRE(due->size() == 1);
    REQUIRE((*due)[0].id == "rem-1");

    // Second claim at same timestamp returns empty (already notified).
    auto due_again = co_await store.claim_due(2000);
    REQUIRE(due_again.has_value());
    REQUIRE(due_again->empty());

    REQUIRE((co_await store.mark_completed("rem-1", true)).has_value());
    REQUIRE((co_await store.remove("rem-2")).has_value());

    auto after = co_await store.list();
    REQUIRE(after.has_value());
    REQUIRE(after->size() == 1);
    REQUIRE((*after)[0].completed);
  });
}
