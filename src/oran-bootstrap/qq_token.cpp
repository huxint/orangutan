#include <oran/bootstrap/qq.hpp>

#include <algorithm>
#include <array>
#include <asio/this_coro.hpp>
#include <charconv>
#include <nlohmann/json.hpp>
#include <oran/async/channel.hpp>
#include <oran/hook/bus.hpp>
#include <sodium/utils.h>

namespace orangutan::bootstrap {
struct QQTokenSource::Impl {
  QQTokenOptions options;
  async::Channel<bool> gate;
  std::size_t callers{};
  std::string token;
  std::chrono::steady_clock::time_point valid_until{};
  explicit Impl(QQTokenOptions value) : options(std::move(value)), gate(options.executor, 1) {}
  ~Impl() {
    sodium_memzero(options.app_secret.data(), options.app_secret.size());
    sodium_memzero(token.data(), token.size());
  }
  async::Awaitable<core::Result<std::string>> get(channel::Conversation conversation) {
    using core::Error;
    if (conversation.platform != channel::Platform::qq || conversation.account != options.app_id)
      co_return std::unexpected(Error{core::ErrorKind::auth, "QQ credential account mismatch"});
    if (callers >= 32)
      co_return std::unexpected(Error{core::ErrorKind::mailbox_overflowed, "QQ credential admission is full"});
    ++callers;
    struct Admission {
      std::size_t& count;
      ~Admission() {
        --count;
      }
    } admission{callers};
    auto locked = co_await gate.receive();
    if (!locked)
      co_return std::unexpected(locked.error());
    struct Release {
      async::Channel<bool>& gate;
      ~Release() {
        static_cast<void>(gate.try_send(true));
      }  // Sole lease returns the one permit.
    } release{gate};
    const auto started = options.now();
    if (!token.empty() && started < valid_until)
      co_return token;
    const std::array required{core::Capability::egress_http};
    const auto decision =
        permission::evaluate(options.rules, "QQToken", options.app_id, required, permission::Mode::strict);
    co_await options.hooks->publish_advisory(
        hook::Event::channel_action,
        hook::ChannelActionPayload{"qq", options.app_id, "QQToken", decision.verdict == permission::Verdict::allow});
    if (decision.verdict != permission::Verdict::allow)
      co_return std::unexpected(Error::permission_denied("QQ token refresh denied"));
    http::BodyRequest request;
    request.method = "POST";
    request.url = "https://bots.qq.com/app/getAppAccessToken";
    request.headers = {{"Content-Type", "application/json"}};
    request.body = nlohmann::json{{"appId", options.app_id}, {"clientSecret", options.app_secret}}.dump();
    request.timeout = std::chrono::seconds{10};
    request.max_bytes = 16384;
    auto result = co_await options.send(std::move(request));
    if (!result)
      co_return std::unexpected(Error{result.error().kind(), "QQ token request failed"});
    if (result->status_code != 200 || result->body.size() > 16384)
      co_return std::unexpected(Error{core::ErrorKind::auth, "QQ token request rejected"});
    auto json = nlohmann::json::parse(result->body);
    if (json.value("code", 0) != 0)
      co_return std::unexpected(Error{core::ErrorKind::auth, "QQ token request rejected"});
    auto value = json.at("access_token").get<std::string>();
    const auto& expiry = json.at("expires_in");
    std::int64_t seconds{};
    if (expiry.is_string()) {
      const auto& text = expiry.get_ref<const std::string&>();
      auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
      if (error != std::errc{} || end != text.data() + text.size())
        co_return std::unexpected(Error{core::ErrorKind::auth, "invalid QQ token lifetime"});
    } else if (expiry.is_number_integer()) {
      seconds = expiry.get<std::int64_t>();
    }
    if (value.empty() || value.size() > 4096 || seconds <= 0 || seconds > 86400)
      co_return std::unexpected(Error{core::ErrorKind::auth, "invalid QQ token response"});
    const auto until = started + std::chrono::seconds{seconds - std::min(std::int64_t{30}, seconds / 10)};
    if (options.now() >= until)
      co_return std::unexpected(Error{core::ErrorKind::auth, "QQ token expired during refresh"});
    sodium_memzero(token.data(), token.size());
    token = std::move(value);
    valid_until = until;
    co_return token;
  }
};

core::Result<std::unique_ptr<QQTokenSource>> QQTokenSource::create(QQTokenOptions options) {
  if (options.app_id.empty() || options.app_id.size() > 64 || options.app_secret.empty() || !options.executor ||
      !options.send || !options.hooks || !options.now)
    return std::unexpected(core::Error::invalid_argument("incomplete QQ token source configuration"));
  auto impl = std::make_unique<Impl>(std::move(options));
  auto ready = impl->gate.try_send(true);
  if (!ready)
    return std::unexpected(ready.error());
  return std::make_unique<QQTokenSource>(std::move(impl), PrivateTag{});
}
QQTokenSource::QQTokenSource(std::unique_ptr<Impl> impl, PrivateTag) : impl_(std::move(impl)) {}
QQTokenSource::~QQTokenSource() = default;
async::Awaitable<core::Result<std::string>> QQTokenSource::get(channel::Conversation conversation) {
  try {
    co_return co_await impl_->get(std::move(conversation));
  } catch (...) {
    // Never copy credential lookup, HTTP, JSON or clock exception text to diagnostics.
  }
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                ? core::Error::cancelled()
                                : core::Error{core::ErrorKind::auth, "QQ credential refresh failed"});
}
}  // namespace orangutan::bootstrap
