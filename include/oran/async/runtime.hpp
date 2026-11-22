#pragma once

#include <cstddef>
#include <memory>

#include <asio/any_io_executor.hpp>
#include <asio/strand.hpp>

#include <oran/core/result.hpp>

namespace orangutan::async {

struct RuntimeConfig {
  std::size_t io_workers{1};
  std::size_t cpu_workers{1};
};

class Runtime {
public:
  explicit Runtime(RuntimeConfig config = RuntimeConfig{});
  ~Runtime();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;

  [[nodiscard]] asio::any_io_executor executor() const noexcept;
  [[nodiscard]] asio::any_io_executor cpu_executor() const noexcept;
  [[nodiscard]] asio::strand<asio::any_io_executor> make_strand() const;

  [[nodiscard]] core::Result<void> run();

  /// Spawn the io workers and return immediately, leaving the runtime running
  /// on its own threads. Use this when another event loop owns the calling
  /// thread (e.g. the Slint desktop UI): `start()` then drive the UI, and pair
  /// teardown with `stop_and_join()`. Returns a `conflict` error if already
  /// running or stopped.
  [[nodiscard]] core::Result<void> start();
  /// Wait for a started runtime to be stopped by its work owner. Unlike
  /// stop_and_join(), this preserves pending coroutine cleanup.
  [[nodiscard]] core::Result<void> join();

  void stop() noexcept;

  /// Request stop and wait for every Runtime-owned IO/CPU worker to exit.
  /// Start-mode owners must call this before destroying state borrowed by
  /// runtime tasks. Any exception captured from an IO worker is returned.
  [[nodiscard]] core::Result<void> stop_and_join();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::async
