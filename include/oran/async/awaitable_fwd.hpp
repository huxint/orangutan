// include/oran/async/awaitable_fwd.hpp — coroutine vocabulary alias.

#pragma once

#include <asio/awaitable.hpp>

namespace orangutan::async {

template <typename T>
using Awaitable = asio::awaitable<T>;

}  // namespace orangutan::async
