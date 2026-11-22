// tests/test-helpers/run_async.hpp — drives one awaitable with a hard timeout.

#pragma once

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include <catch2/catch_test_macros.hpp>

namespace orangutan::tests {

template <typename Fn>
void run_async(Fn&& fn, std::chrono::steady_clock::duration timeout_duration = std::chrono::seconds{1}) {
  asio::io_context io;
  std::exception_ptr failure;
  bool finished = false;

  asio::steady_timer timeout{io};
  timeout.expires_after(timeout_duration);
  timeout.async_wait([&](const asio::error_code& ec) {
    if (!ec && !finished) {
      failure = std::make_exception_ptr(std::runtime_error{"async test timed out"});
      io.stop();
    }
  });

  asio::co_spawn(io, std::forward<Fn>(fn)(io), [&](std::exception_ptr ep) {
    finished = true;
    if (ep) {
      failure = ep;
    }
    timeout.cancel();
    io.stop();
  });

  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(finished);
}

}  // namespace orangutan::tests
