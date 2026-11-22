#pragma once

#include <oran/bootstrap/qq.hpp>

namespace orangutan::bootstrap {
/// Sensitive host-owned value; never include the secret in prompts or diagnostics.
struct QQCredentials {
  std::string app_id;
  std::string app_secret;
  std::string user_openid;
};

struct QQConnectOptions {
  QQHttpSend send;
  hook::Bus* hooks{};
  permission::RuleSet rules;
  /// Display only this official connect URL. The callback must finish borrowed work.
  std::function<async::Awaitable<core::Result<void>>(std::string)> display;
  std::chrono::milliseconds timeout{std::chrono::minutes{5}};
  std::chrono::milliseconds poll_interval{std::chrono::seconds{2}};
};

/// One bounded QR authorization, with at most three QR tasks. The host retains
/// options and borrowed services until completion and owns credential persistence.
[[nodiscard]] async::Awaitable<core::Result<QQCredentials>> connect_qq(QQConnectOptions& options);
}  // namespace orangutan::bootstrap
