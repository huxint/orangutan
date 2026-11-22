#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <asio/any_io_executor.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/http/client.hpp>

namespace orangutan::bootstrap {
struct QQWebhookRequest {
  std::string timestamp;
  std::string signature;
  std::string body;
};

/// Pure verification over the original bytes. Timestamp skew is limited to five minutes.
[[nodiscard]] core::Result<void>
verify_qq_webhook(const QQWebhookRequest& request, std::string_view app_secret, std::chrono::sys_seconds now);

/// Success means durably and idempotently queued by event identity, never merely scheduled.
using QQEnqueue = std::function<async::Awaitable<core::Result<void>>(channel::Message)>;

/// The HTTP host bounds reads and maps errors to non-success HTTP statuses. Authenticated
/// dispatch ACKs follow enqueue completion; the host owns queue workers and an outbox.
/// The secret, hook bus, rules and enqueue port must outlive the returned operation.
[[nodiscard]] async::Awaitable<core::Result<channel::Response>> accept_qq_webhook(QQWebhookRequest request,
                                                                                  channel::Account account,
                                                                                  std::string_view app_secret,
                                                                                  std::chrono::sys_seconds now,
                                                                                  hook::Bus& hooks,
                                                                                  const permission::RuleSet& rules,
                                                                                  const QQEnqueue& enqueue);

using QQHttpSend = std::function<async::Awaitable<core::Result<http::BodyResponse>>(http::BodyRequest)>;
struct QQTokenOptions {
  std::string app_id;
  /// Resolved by the host from its secret reference; never serialize this options value.
  std::string app_secret;
  asio::any_io_executor executor;
  QQHttpSend send;
  hook::Bus* hooks{};
  permission::RuleSet rules;
  std::function<std::chrono::steady_clock::time_point()> now{std::chrono::steady_clock::now};
};

/// One account on one coordinating strand. Refreshes are joined and serialized,
/// with at most 32 callers admitted. Await callers before releasing the source.
class QQTokenSource {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<QQTokenSource>> create(QQTokenOptions options);
  ~QQTokenSource();
  QQTokenSource(const QQTokenSource&) = delete;
  QQTokenSource& operator=(const QQTokenSource&) = delete;
  [[nodiscard]] async::Awaitable<core::Result<std::string>> get(channel::Conversation conversation);
  class PrivateTag {
    PrivateTag() = default;
    friend class QQTokenSource;
  };
  struct Impl;
  QQTokenSource(std::unique_ptr<Impl> impl, PrivateTag);

private:
  std::unique_ptr<Impl> impl_;
};
}  // namespace orangutan::bootstrap
