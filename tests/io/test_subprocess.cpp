#include <chrono>
#include <filesystem>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/async/runtime.hpp>
#include <oran/core/error.hpp>
#include <oran/io/subprocess.hpp>

#include "../test-helpers/run_async.hpp"

namespace io = orangutan::io;
namespace core = orangutan::core;
namespace async = orangutan::async;
namespace test = orangutan::tests;

TEST_CASE("run_subprocess captures stdout, stderr, exit code and working directory", "[unit][io][subprocess]") {
  test::run_async([](asio::io_context& ctx) -> async::Awaitable<void> {
    const auto tmp = std::filesystem::temp_directory_path();
    auto res = co_await io::run_subprocess(ctx.get_executor(),
                                           io::SubprocessOptions{
                                               .command = "pwd && echo hello-out && echo hello-err >&2",
                                               .working_directory = tmp.string(),
                                               .timeout = std::chrono::seconds{5},
                                           });
    REQUIRE(res.has_value());
    REQUIRE(res->exit_code == 0);
    REQUIRE_FALSE(res->timed_out);
    REQUIRE(res->stdout_text.contains("hello-out"));
    REQUIRE(res->stderr_text.contains("hello-err"));
  });
}

TEST_CASE("run_subprocess enforces output caps and marks truncation", "[unit][io][subprocess]") {
  test::run_async([](asio::io_context& ctx) -> async::Awaitable<void> {
    auto res = co_await io::run_subprocess(ctx.get_executor(),
                                           io::SubprocessOptions{
                                               .command = "head -c 4096 /dev/zero | tr '\\0' 'A'",
                                               .timeout = std::chrono::seconds{5},
                                               .max_output_bytes = 512,
                                           });
    REQUIRE(res.has_value());
    REQUIRE(res->exit_code == 0);
    REQUIRE(res->stdout_truncated);
    REQUIRE(res->stdout_text.size() == 512);
  });
}

TEST_CASE("run_subprocess terminates timed-out commands cleanly", "[unit][io][subprocess]") {
  test::run_async([](asio::io_context& ctx) -> async::Awaitable<void> {
    auto res = co_await io::run_subprocess(ctx.get_executor(),
                                           io::SubprocessOptions{
                                               .command = "echo started && sleep 5",
                                               .timeout = std::chrono::milliseconds{150},
                                           });
    REQUIRE(res.has_value());
    REQUIRE(res->timed_out);
    REQUIRE(res->exit_code == -1);
    REQUIRE(res->stdout_text.contains("started"));
  });
}

TEST_CASE("run_subprocess stops promptly on coroutine cancellation", "[unit][io][subprocess]") {
  async::Runtime runtime{async::RuntimeConfig{.io_workers = 1, .cpu_workers = 2}};
  auto started = runtime.start();
  REQUIRE(started.has_value());

  std::optional<core::Result<io::SubprocessResult>> outcome;
  asio::cancellation_signal signal;
  asio::co_spawn(
      runtime.executor(),
      [&]() -> async::Awaitable<void> {
        asio::steady_timer cancel_timer{runtime.executor(), std::chrono::milliseconds{120}};
        cancel_timer.async_wait([&signal](std::error_code ec) {
          if (!ec) {
            signal.emit(asio::cancellation_type::terminal);
          }
        });
        outcome = co_await asio::co_spawn(
            runtime.executor(),
            io::run_subprocess(runtime.cpu_executor(),
                               io::SubprocessOptions{
                                   .command = "sleep 10",
                                   .timeout = std::chrono::seconds{10},
                               }),
            asio::bind_cancellation_slot(signal.slot(), asio::use_awaitable));
        runtime.stop();
      },
      asio::detached);

  REQUIRE(runtime.join().has_value());
  REQUIRE(outcome.has_value());
  REQUIRE_FALSE(outcome->has_value());
  REQUIRE(outcome->error().kind() == core::ErrorKind::cancelled);
}
