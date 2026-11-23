#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::io {

struct SubprocessOptions {
  std::string command;
  std::string working_directory{};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  std::size_t max_output_bytes{64U * 1024U};
};

struct SubprocessResult {
  int exit_code{0};
  std::string stdout_text{};
  std::string stderr_text{};
  bool stdout_truncated{false};
  bool stderr_truncated{false};
  bool timed_out{false};
  std::chrono::milliseconds duration{0};
};

/// Execute a shell command in an isolated process group on the blocking worker
/// executor. Cancellation or timeout terminates the entire process group and
/// reaps the child before returning.
[[nodiscard]] async::Awaitable<core::Result<SubprocessResult>>
run_subprocess(asio::any_io_executor executor, SubprocessOptions options);

}  // namespace orangutan::io
