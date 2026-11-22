#include <oran/io/blocking.hpp>

#include <asio/thread_pool.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace io = orangutan::io;

TEST_CASE("blocking operation executes on the supplied worker pool", "[unit][io][blocking]") {
  asio::thread_pool workers{1};
  orangutan::tests::run_async([&workers](asio::io_context&) -> async::Awaitable<void> {
    auto operation = [&workers](std::stop_token) -> core::Result<bool> {
      return workers.get_executor().running_in_this_thread();
    };
    const auto result = co_await io::run_blocking(workers.get_executor(), std::move(operation));

    REQUIRE(result);
    CHECK(*result);
  });
  workers.join();
}
