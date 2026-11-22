// bench/async/scenarios/channel_ping_pong.cpp
//
// A-vs-B comparison: bounded Channel<T> coroutine handoff vs. direct coroutine
// posts. This keeps the first async backpressure primitive honest without
// pretending there is a historical baseline yet.

#include <nanobench.h>

#include <expected>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/channel.hpp>

namespace orangutan::bench {

namespace {

constexpr int kMessages = 32;

[[gnu::noinline]] int run_direct_post_loop() {
  asio::io_context io;
  int sum = 0;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kMessages; ++i) {
          sum += i;
          co_await asio::post(io, asio::use_awaitable);
        }
        io.stop();
        co_return;
      },
      asio::detached);

  io.run();
  return sum;
}

[[gnu::noinline]] int run_channel_ping_pong() {
  asio::io_context io;
  async::Channel<int> channel{io.get_executor(), 1};
  int sum = 0;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        for (int i = 0; i < kMessages; ++i) {
          auto sent = co_await channel.send(i);
          if (!sent) {
            io.stop();
            co_return;
          }
        }
        channel.close();
        co_return;
      },
      asio::detached);

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        while (true) {
          auto value = co_await channel.receive();
          if (!value) {
            break;
          }
          sum += *value;
        }
        io.stop();
        co_return;
      },
      asio::detached);

  io.run();
  return sum;
}

}  // namespace

void register_channel_ping_pong(ankerl::nanobench::Bench& bench) {
  bench.run("async.direct_post_loop", [] {
    auto sum = run_direct_post_loop();
    ankerl::nanobench::doNotOptimizeAway(sum);
  });
  bench.run("async.channel_ping_pong", [] {
    auto sum = run_channel_ping_pong();
    ankerl::nanobench::doNotOptimizeAway(sum);
  });
}

}  // namespace orangutan::bench
