// src/oran-async/sleep.cpp — cancel-aware timer helper.

#include <oran/async/sleep.hpp>

#include <expected>

#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/core/error.hpp>

namespace orangutan::async {

Awaitable<core::Result<void>> sleep_for(asio::any_io_executor executor, std::chrono::steady_clock::duration duration) {
  auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  asio::steady_timer timer{std::move(executor)};
  timer.expires_after(duration);

  asio::error_code ec;
  co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

  if (ec == asio::error::operation_aborted) {
    co_return std::unexpected(core::Error::cancelled());
  }
  if (ec) {
    co_return std::unexpected(core::Error::internal("timer wait failed").with("asio_error", ec.message()));
  }
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  co_return core::Result<void>{};
}

}  // namespace orangutan::async
