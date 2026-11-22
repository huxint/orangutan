// tests/async/test_async.cpp — Runtime, sleep_for, and Channel coverage.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <expected>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>

#include "../test-helpers/run_async.hpp"

using namespace std::chrono_literals;

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace test = orangutan::tests;

TEST_CASE("Runtime runs work on its executor until stopped", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};
  std::atomic_bool ran = false;

  asio::co_spawn(
      runtime.executor(),
      [&]() -> async::Awaitable<void> {
        ran = true;
        runtime.stop();
        co_return;
      },
      asio::detached);

  auto result = runtime.run();
  REQUIRE(result.has_value());
  REQUIRE(ran.load());
}

TEST_CASE("Runtime::start runs work off the calling thread without blocking", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};

  std::promise<void> ran;
  auto ran_future = ran.get_future();

  // start() spawns the io workers and returns immediately (unlike run(), which
  // blocks). The posted work therefore runs on a Runtime-owned worker thread.
  auto started = runtime.start();
  REQUIRE(started.has_value());

  asio::post(runtime.executor(), [&] { ran.set_value(); });
  REQUIRE(ran_future.wait_for(1s) == std::future_status::ready);

  runtime.stop();
}

TEST_CASE("Runtime::start rejects a second start and a later run", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};

  REQUIRE(runtime.start().has_value());

  auto again = runtime.start();
  REQUIRE_FALSE(again.has_value());
  REQUIRE(again.error().kind() == core::ErrorKind::conflict);

  auto blocking = runtime.run();
  REQUIRE_FALSE(blocking.has_value());
  REQUIRE(blocking.error().kind() == core::ErrorKind::conflict);

  runtime.stop();
}

TEST_CASE("Runtime::stop_and_join waits for active workers", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::atomic_bool finished = false;

  REQUIRE(runtime.start().has_value());
  asio::post(runtime.executor(), [&] {
    entered.set_value();
    std::this_thread::sleep_for(20ms);
    finished.store(true);
  });
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);

  REQUIRE(runtime.stop_and_join().has_value());
  REQUIRE(finished.load());
}

TEST_CASE("Runtime::stop_and_join reports start-mode worker exceptions", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};
  std::promise<void> entered;
  auto entered_future = entered.get_future();

  asio::post(runtime.executor(), [&] {
    entered.set_value();
    throw std::runtime_error{"start boom"};
  });
  REQUIRE(runtime.start().has_value());
  REQUIRE(entered_future.wait_for(1s) == std::future_status::ready);

  auto stopped = runtime.stop_and_join();
  REQUIRE_FALSE(stopped.has_value());
  REQUIRE(stopped.error().kind() == core::ErrorKind::internal);
  REQUIRE(std::ranges::any_of(stopped.error().context(), [](const auto& entry) {
    return entry.first == "reason" && entry.second == "start boom";
  }));
}

TEST_CASE("Runtime rejects a second run after stop", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 1}};

  asio::post(runtime.executor(), [&] { runtime.stop(); });

  auto first = runtime.run();
  REQUIRE(first.has_value());

  auto second = runtime.run();
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error().kind() == core::ErrorKind::conflict);
  REQUIRE(second.error().message() == "runtime has already stopped");
}

TEST_CASE("Runtime reports executor handler exceptions", "[unit][async][runtime]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 2, .cpu_workers = 1}};

  asio::post(runtime.executor(), [] { throw std::runtime_error{"boom"}; });

  auto result = runtime.run();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind() == core::ErrorKind::internal);
  REQUIRE(result.error().message() == "runtime io worker failed");
  REQUIRE(std::ranges::any_of(result.error().context(),
                              [](const auto& entry) { return entry.first == "reason" && entry.second == "boom"; }));
}

TEST_CASE("sleep_for completes on timer expiry", "[unit][async][sleep]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto result = co_await async::sleep_for(io.get_executor(), 1ms);
    REQUIRE(result.has_value());
  });
}

TEST_CASE("sleep_for reports cancellation", "[unit][async][sleep]") {
  asio::io_context io;
  asio::cancellation_signal signal;
  std::optional<core::Result<void>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<void>> { co_return co_await async::sleep_for(io.get_executor(), 1s); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<void> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("TaskGroup enforces active capacity and releases it on completion", "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto created = async::TaskGroup::create(io.get_executor(), async::TaskGroupOptions{.max_tasks = 1});
    REQUIRE(created.has_value());
    auto group = std::move(*created);
    async::Channel<std::monostate> gate{io.get_executor(), 1};

    REQUIRE(group
                .spawn("first",
                       [&]() -> async::Awaitable<core::Result<void>> {
                         auto opened = co_await gate.receive();
                         if (!opened) {
                           co_return std::unexpected(std::move(opened).error());
                         }
                         co_return core::Result<void>{};
                       })
                .has_value());

    auto full = group.spawn("second", []() -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; });
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().kind() == core::ErrorKind::mailbox_overflowed);

    REQUIRE(gate.try_send(std::monostate{}).has_value());
    while (group.active_tasks() != 0) {
      co_await asio::post(io, asio::use_awaitable);
    }
    REQUIRE(group.spawn("second", []() -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; })
                .has_value());

    auto report = co_await group.join();
    REQUIRE(report.has_value());
    CHECK(report->tasks.size() == 2);
    CHECK(report->succeeded() == 2);
  });
}

TEST_CASE("TaskGroup reports child errors and exceptions in spawn order", "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto created = async::TaskGroup::create(io.get_executor());
    REQUIRE(created.has_value());
    auto group = std::move(*created);

    REQUIRE(group.spawn("ok", []() -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; })
                .has_value());
    REQUIRE(group
                .spawn("error",
                       []() -> async::Awaitable<core::Result<void>> {
                         co_return std::unexpected(core::Error::invalid_argument("bad child"));
                       })
                .has_value());
    REQUIRE(group
                .spawn("exception",
                       []() -> async::Awaitable<core::Result<void>> {
                         throw std::runtime_error{"boom"};
                         co_return core::Result<void>{};
                       })
                .has_value());

    auto report = co_await group.join();
    REQUIRE(report.has_value());
    REQUIRE(report->tasks.size() == 3);
    CHECK(report->tasks[0].name == "ok");
    CHECK(report->tasks[0].status == async::TaskOutcomeStatus::succeeded);
    CHECK(report->tasks[1].name == "error");
    CHECK(report->tasks[1].status == async::TaskOutcomeStatus::failed);
    REQUIRE(report->tasks[1].error.has_value());
    CHECK(report->tasks[1].error->kind() == core::ErrorKind::invalid_argument);
    CHECK(report->tasks[2].name == "exception");
    CHECK(report->tasks[2].status == async::TaskOutcomeStatus::failed);
    REQUIRE(report->tasks[2].error.has_value());
    CHECK(report->tasks[2].error->kind() == core::ErrorKind::internal);
    CHECK(report->failed() == 2);
    CHECK_FALSE(report->all_succeeded());
  });
}

TEST_CASE("TaskGroup request_stop cancels children and join drains them", "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto created = async::TaskGroup::create(io.get_executor());
    REQUIRE(created.has_value());
    auto group = std::move(*created);
    std::atomic_bool started{false};

    REQUIRE(group
                .spawn("waiting",
                       [&]() -> async::Awaitable<core::Result<void>> {
                         started.store(true, std::memory_order_release);
                         co_return co_await async::sleep_for(io.get_executor(), 1s);
                       })
                .has_value());
    while (!started.load(std::memory_order_acquire)) {
      co_await asio::post(io, asio::use_awaitable);
    }

    group.request_stop();
    auto rejected =
        group.spawn("late", []() -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; });
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().kind() == core::ErrorKind::conflict);
    auto report = co_await group.join();
    REQUIRE(report.has_value());
    REQUIRE(report->tasks.size() == 1);
    CHECK(report->tasks.front().name == "waiting");
    CHECK(report->tasks.front().status == async::TaskOutcomeStatus::cancelled);
    CHECK(report->cancelled() == 1);
    CHECK(group.active_tasks() == 0);
  });
}

TEST_CASE("TaskGroup state outlives a destroyed facade until its cancellation laggard finishes",
          "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto created = async::TaskGroup::create(io.get_executor());
    REQUIRE(created.has_value());
    auto group = std::optional<async::TaskGroup>{std::move(*created)};
    async::Channel<std::monostate> release{io.get_executor(), 1};
    bool laggard_started = false;
    bool laggard_finished = false;

    REQUIRE(group
                ->spawn("laggard",
                        [&]() -> async::Awaitable<core::Result<void>> {
                          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
                          laggard_started = true;
                          auto released = co_await release.receive();
                          if (!released) {
                            co_return std::unexpected(std::move(released).error());
                          }
                          laggard_finished = true;
                          co_return core::Result<void>{};
                        })
                .has_value());

    while (!laggard_started) {
      co_await asio::post(io, asio::use_awaitable);
    }
    group.reset();
    CHECK_FALSE(laggard_finished);
    REQUIRE(release.try_send(std::monostate{}).has_value());
    while (!laggard_finished) {
      co_await asio::post(io, asio::use_awaitable);
    }
    CHECK(laggard_finished);
  });
}

TEST_CASE("TaskGroup accepts move-only child ownership", "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto created = async::TaskGroup::create(io.get_executor());
    REQUIRE(created.has_value());
    auto group = std::move(*created);
    auto owned = std::make_unique<int>(42);

    REQUIRE(group
                .spawn("move-only",
                       [owned = std::move(owned)]() -> async::Awaitable<core::Result<void>> {
                         CHECK(*owned == 42);
                         co_return core::Result<void>{};
                       })
                .has_value());
    auto report = co_await group.join();
    REQUIRE(report.has_value());
    CHECK(report->succeeded() == 1);
  });
}

TEST_CASE("TaskGroup bounds and drains completed outcome retention", "[unit][async][task-group]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    auto invalid =
        async::TaskGroup::create(io.get_executor(), async::TaskGroupOptions{.max_tasks = 1, .max_completed = 0});
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().kind() == core::ErrorKind::invalid_argument);

    auto created =
        async::TaskGroup::create(io.get_executor(), async::TaskGroupOptions{.max_tasks = 3, .max_completed = 2});
    REQUIRE(created.has_value());
    auto group = std::move(*created);
    for (const auto* name : {"first", "second", "third"}) {
      REQUIRE(group.spawn(name, []() -> async::Awaitable<core::Result<void>> { co_return core::Result<void>{}; })
                  .has_value());
    }
    while (group.active_tasks() != 0) {
      co_await asio::post(io, asio::use_awaitable);
    }

    auto retained = group.drain_completed();
    REQUIRE(retained.tasks.size() == 2);
    CHECK(retained.tasks[0].name == "second");
    CHECK(retained.tasks[1].name == "third");
    CHECK(retained.outcomes_dropped == 1);
    CHECK_FALSE(retained.all_succeeded());

    auto drained = group.drain_completed();
    CHECK(drained.tasks.empty());
    CHECK(drained.outcomes_dropped == 0);

    auto joined = co_await group.join();
    REQUIRE(joined.has_value());
    CHECK(joined->tasks.empty());
    CHECK(joined->outcomes_dropped == 0);
  });
}

TEST_CASE("Channel sends and receives FIFO values", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 2};

    auto sent_first = co_await channel.send(1);
    auto sent_second = co_await channel.send(2);
    REQUIRE(sent_first.has_value());
    REQUIRE(sent_second.has_value());

    auto first = co_await channel.receive();
    auto second = co_await channel.receive();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == 1);
    REQUIRE(*second == 2);
  });
}

TEST_CASE("Channel try_send reports overflow when full", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 1};

  REQUIRE(channel.try_send(7).has_value());

  auto overflow = channel.try_send(8);
  REQUIRE_FALSE(overflow.has_value());
  REQUIRE(overflow.error().kind() == core::ErrorKind::mailbox_overflowed);
}

TEST_CASE("Channel try_receive reports empty without waiting and drains FIFO values", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 2};

  auto empty = channel.try_receive();
  REQUIRE(empty.has_value());
  REQUIRE_FALSE(empty->has_value());

  REQUIRE(channel.try_send(7).has_value());
  REQUIRE(channel.try_send(8).has_value());

  auto first = channel.try_receive();
  auto second = channel.try_receive();
  auto empty_again = channel.try_receive();

  REQUIRE(first.has_value());
  REQUIRE(first->has_value());
  REQUIRE(**first == 7);
  REQUIRE(second.has_value());
  REQUIRE(second->has_value());
  REQUIRE(**second == 8);
  REQUIRE(empty_again.has_value());
  REQUIRE_FALSE(empty_again->has_value());
}

TEST_CASE("Channel try_receive drains buffered values before reporting closed", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 1};

  REQUIRE(channel.try_send(7).has_value());
  channel.close();

  auto buffered = channel.try_receive();
  auto closed = channel.try_receive();

  REQUIRE(buffered.has_value());
  REQUIRE(buffered->has_value());
  REQUIRE(**buffered == 7);
  REQUIRE_FALSE(closed.has_value());
  REQUIRE(closed.error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("Channel try_receive completes a pending sender without awaiting", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 0};
    std::optional<core::Result<void>> send_result;

    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          send_result = co_await channel.send(42);
          co_return;
        },
        asio::detached);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(send_result.has_value());

    auto received = channel.try_receive();
    REQUIRE(received.has_value());
    REQUIRE(received->has_value());
    REQUIRE(**received == 42);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE(send_result.has_value());
    REQUIRE(send_result->has_value());
  });
}

TEST_CASE("Channel pending send completes when receive frees capacity", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 1};
    REQUIRE(channel.try_send(1).has_value());

    std::optional<core::Result<void>> send_result;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<void> {
          send_result = co_await channel.send(2);
          co_return;
        },
        asio::detached);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE_FALSE(send_result.has_value());

    auto first = co_await channel.receive();
    REQUIRE(first.has_value());
    REQUIRE(*first == 1);

    co_await asio::post(io, asio::use_awaitable);
    REQUIRE(send_result.has_value());
    REQUIRE(send_result->has_value());

    auto second = co_await channel.receive();
    REQUIRE(second.has_value());
    REQUIRE(*second == 2);
  });
}

TEST_CASE("Channel close drains buffered values then reports closed", "[unit][async][channel]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    async::Channel<int> channel{io.get_executor(), 2};

    REQUIRE(channel.try_send(1).has_value());
    REQUIRE(channel.try_send(2).has_value());
    channel.close();

    auto first = co_await channel.receive();
    auto second = co_await channel.receive();
    auto closed = co_await channel.receive();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(*first == 1);
    REQUIRE(*second == 2);
    REQUIRE_FALSE(closed.has_value());
    REQUIRE(closed.error().kind() == core::ErrorKind::cancelled);
  });
}

TEST_CASE("Channel receive observes coroutine cancellation", "[unit][async][channel]") {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 0};
  asio::cancellation_signal signal;
  std::optional<core::Result<int>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<int>> { co_return co_await channel.receive(); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<int> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

// Slice 31 regression: a Channel created on executor A but driven by a
// coroutine spawned on strand B must resume the coroutine on strand B.
// Before the fix, completions were posted to the channel's executor (A),
// which would race against any other work on B and quietly violate the
// strand's serialization guarantee.
TEST_CASE("Channel resumes coroutine on its associated executor (not the channel's)",
          "[unit][async][channel][regression]") {
  asio::thread_pool pool{2};
  auto strand_a = asio::make_strand(pool);
  auto strand_b = asio::make_strand(pool);

  async::Channel<int> channel{strand_a, 1};

  std::atomic_bool ran_on_strand_b = false;
  std::atomic_bool finished = false;

  asio::co_spawn(
      strand_b,
      [&]() -> async::Awaitable<void> {
        // Ask asio for the executor the continuation will resume on. With
        // the slice-31 fix, this is strand_b; before the fix, the channel
        // ignored the handler's associated executor and posted to strand_a.
        auto received = co_await channel.receive();
        REQUIRE(received.has_value());
        REQUIRE(*received == 7);
        // running_in_this_thread() is the cheapest cross-executor check —
        // when on strand_b's thread, only strand_b's posted work runs.
        ran_on_strand_b = strand_b.running_in_this_thread();
        finished = true;
        co_return;
      },
      asio::detached);

  // Send via the channel's executor; this enqueues the value and triggers
  // pump_locked, which would have posted the receiver's completion to
  // strand_a under the old code.
  asio::post(strand_a, [&] { REQUIRE(channel.try_send(7).has_value()); });

  pool.join();

  REQUIRE(finished.load());
  REQUIRE(ran_on_strand_b.load());
}
