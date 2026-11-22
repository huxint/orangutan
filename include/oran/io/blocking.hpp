#pragma once

#include <exception>
#include <expected>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <asio/any_io_executor.hpp>
#include <asio/associated_cancellation_slot.hpp>
#include <asio/associated_executor.hpp>
#include <asio/async_result.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::io {

/// Execute on the worker executor and resume on the caller's executor.
/// Cancellation requests the operation's stop token. An operation already in
/// progress retains its captures until it returns; committed results are preserved.
template <typename Fn>
  requires core::detail::is_result_v<std::invoke_result_t<Fn&, std::stop_token>>
[[nodiscard]] async::Awaitable<std::invoke_result_t<Fn&, std::stop_token>> run_blocking(asio::any_io_executor executor,
                                                                                        Fn fn) {
  using Result = std::invoke_result_t<Fn&, std::stop_token>;
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }

  auto token = asio::use_awaitable;
  co_return co_await asio::async_initiate<decltype(token), void(Result)>(
      [executor = std::move(executor), fn = std::move(fn)](auto complete) mutable {
        auto caller = asio::get_associated_executor(complete);
        auto slot = asio::get_associated_cancellation_slot(complete);
        std::stop_source stop;
        if (slot.is_connected()) {
          slot.assign([stop](asio::cancellation_type type) mutable {
            if (type != asio::cancellation_type::none) {
              stop.request_stop();
            }
          });
        }
        asio::post(executor, [caller, slot, stop, fn = std::move(fn), complete = std::move(complete)]() mutable {
          auto execute = [&stop, &fn]() -> Result {
            if (stop.stop_requested()) {
              return std::unexpected(core::Error::cancelled());
            }
            try {
              return fn(stop.get_token());
            } catch (const std::exception& error) {
              return std::unexpected(core::Error::io("blocking operation failed").with("cause", error.what()));
            }
          };
          auto result = execute();
          asio::post(caller, [slot, result = std::move(result), complete = std::move(complete)]() mutable {
            if (slot.is_connected()) {
              slot.clear();
            }
            complete(std::move(result));
          });
        });
      },
      token);
}

}  // namespace orangutan::io
