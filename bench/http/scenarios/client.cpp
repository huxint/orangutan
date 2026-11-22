// bench/http/scenarios/client.cpp
//
// A-vs-B comparison: request validation through the public client vs. client
// construction baseline. The scenario avoids network IO so it stays stable in CI.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/thread_pool.hpp>

#include <oran/http.hpp>

namespace orangutan::bench {
namespace http = orangutan::http;

namespace {

[[gnu::noinline]] std::size_t construct_client(asio::any_io_executor blocking_executor) {
  auto client = http::Client{std::move(blocking_executor)};
  ankerl::nanobench::doNotOptimizeAway(&client);
  return sizeof(client);
}

[[gnu::noinline]] std::size_t validate_body_request(asio::any_io_executor blocking_executor) {
  asio::io_context context;
  auto client = http::Client{std::move(blocking_executor)};
  std::size_t value = 0;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<void> {
        auto response = co_await client.send(http::BodyRequest{
            .method = "TRACE",
            .url = "http://127.0.0.1/",
            .headers = {},
            .body = {},
            .timeout = std::chrono::milliseconds{30000},
        });
        if (response) {
          std::abort();
        }
        value = static_cast<std::size_t>(response.error().kind());
        context.stop();
        co_return;
      },
      asio::detached);

  context.run();
  return value;
}

}  // namespace

void register_http_client(ankerl::nanobench::Bench& bench) {
  asio::thread_pool blocking{1};
  auto executor = blocking.get_executor();

  bench.run("http.construct_client", [&] {
    const auto value = construct_client(executor);
    ankerl::nanobench::doNotOptimizeAway(value);
  });

  bench.minEpochIterations(2'000);
  bench.run("http.validate_body_request", [&] {
    const auto value = validate_body_request(executor);
    ankerl::nanobench::doNotOptimizeAway(value);
  });

  blocking.join();
}

}  // namespace orangutan::bench
