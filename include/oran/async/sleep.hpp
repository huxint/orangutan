// include/oran/async/sleep.hpp — cancel-aware timer helper.

#pragma once

#include <chrono>
#include <utility>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::async {

[[nodiscard]] Awaitable<core::Result<void>> sleep_for(asio::any_io_executor executor,
                                                      std::chrono::steady_clock::duration duration);

template <typename Rep, typename Period>
[[nodiscard]] Awaitable<core::Result<void>> sleep_for(asio::any_io_executor executor,
                                                      std::chrono::duration<Rep, Period> duration) {
  return sleep_for(std::move(executor), std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration));
}

}  // namespace orangutan::async
