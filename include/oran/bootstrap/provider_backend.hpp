#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/core/result.hpp>
#include <oran/provider/protocol_transport.hpp>

namespace orangutan::config {
class Config;
}  // namespace orangutan::config

namespace orangutan::bootstrap {

struct HttpProviderBackendOptions {
  asio::any_io_executor blocking_executor{};
  std::chrono::milliseconds request_timeout{600000};
  /// Maximum response wire bytes per provider request (streaming and error
  /// bodies alike), mirroring `config.runtime.stream.max_bytes`. Exceeding
  /// the budget aborts the transfer with an IO error.
  std::uint64_t max_stream_bytes{16 * 1024 * 1024};
  std::string route_name{"default"};
  provider::SecretLookup secrets{};
};

class HttpProviderBackend {
public:
  [[nodiscard]] static core::Result<HttpProviderBackend> build(const config::Config& config,
                                                               HttpProviderBackendOptions options);

  HttpProviderBackend(const HttpProviderBackend&) = delete;
  HttpProviderBackend& operator=(const HttpProviderBackend&) = delete;
  HttpProviderBackend(HttpProviderBackend&&) noexcept;
  HttpProviderBackend& operator=(HttpProviderBackend&&) noexcept;
  ~HttpProviderBackend();

  [[nodiscard]] provider::System& system() noexcept;
  [[nodiscard]] const provider::System& system() const noexcept;
  [[nodiscard]] const provider::Route& route() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit HttpProviderBackend(std::unique_ptr<Impl> impl) noexcept;
};

}  // namespace orangutan::bootstrap
