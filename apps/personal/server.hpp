#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <asio/any_io_executor.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>

#include "reminders.hpp"

namespace orangutan::personal {

struct PersonalServerOptions {
  asio::any_io_executor strand;
  asio::any_io_executor blocking_executor;
  bootstrap::RuntimeAssembly* assembly{nullptr};
  const config::Config* config{nullptr};
  provider::System* provider{nullptr};
  provider::Route route{};
  ReminderStore* reminders{nullptr};
  std::string workspace_path;
  std::string state_dir;
  std::string bind_host{"127.0.0.1"};
  std::uint16_t port{18790};
  bool demo_mode{false};
};

/// Loopback-only HTTP/1.1 + SSE bridge exposing the C++26 Personal Agent
/// runtime (`AgentSession`, interactive `permission_ask_rendered` approvals,
/// SQLite `Fts5Backend` long-term memory, and SQLite `ReminderStore`) to local
/// clients such as `apps/desktop`.
class PersonalServer {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<PersonalServer>> create(PersonalServerOptions options);
  ~PersonalServer();

  PersonalServer(const PersonalServer&) = delete;
  PersonalServer& operator=(const PersonalServer&) = delete;

  [[nodiscard]] std::uint16_t bound_port() const noexcept;
  [[nodiscard]] async::Awaitable<core::Result<void>> run();
  void stop() noexcept;

private:
  class Impl;
  explicit PersonalServer(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::personal
