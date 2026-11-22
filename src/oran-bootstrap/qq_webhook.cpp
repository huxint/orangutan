#include <oran/bootstrap/qq.hpp>

#include <array>
#include <asio/this_coro.hpp>
#include <charconv>
#include <nlohmann/json.hpp>
#include <oran/hook/bus.hpp>
#include <ranges>
#include <sodium.h>

namespace orangutan::bootstrap {
namespace {
using core::Error;
using Json = nlohmann::json;
struct Keys {
  std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
  std::array<unsigned char, crypto_sign_SECRETKEYBYTES> private_key{};
  explicit Keys(std::string_view secret) {
    for (const auto i : std::views::iota(std::size_t{}, seed.size()))
      seed[i] = static_cast<unsigned char>(secret[i % secret.size()]);
    crypto_sign_seed_keypair(public_key.data(), private_key.data(), seed.data());
  }
  ~Keys() {
    sodium_memzero(seed.data(), seed.size());
    sodium_memzero(private_key.data(), private_key.size());
  }
};
channel::Response response(Json body) {
  return {200, body.dump(), {}};
}
}  // namespace

core::Result<void>
verify_qq_webhook(const QQWebhookRequest& request, std::string_view secret, std::chrono::sys_seconds now) {
  if (secret.empty() || sodium_init() < 0)
    return std::unexpected(Error::config("QQ webhook credential unavailable"));
  if (request.body.size() > 1024 * 1024 || request.timestamp.empty() || request.timestamp.size() > 16 ||
      request.signature.size() != crypto_sign_BYTES * 2)
    return std::unexpected(Error{core::ErrorKind::auth, "invalid QQ webhook signature input"});
  std::int64_t timestamp{};
  const auto* first = request.timestamp.data();
  auto [end, error] = std::from_chars(first, first + request.timestamp.size(), timestamp);
  const auto current = now.time_since_epoch().count();
  if (error != std::errc{} || end != first + request.timestamp.size() || timestamp < 0 || current < 0 ||
      (timestamp > current ? timestamp - current : current - timestamp) > 300)
    return std::unexpected(Error{core::ErrorKind::auth, "expired or invalid QQ webhook timestamp"});
  std::array<unsigned char, crypto_sign_BYTES> signature{};
  if (sodium_hex2bin(signature.data(),
                     signature.size(),
                     request.signature.data(),
                     request.signature.size(),
                     nullptr,
                     nullptr,
                     nullptr) != 0)
    return std::unexpected(Error{core::ErrorKind::auth, "invalid QQ webhook signature"});
  Keys keys{secret};
  const auto signed_bytes = request.timestamp + request.body;
  if (crypto_sign_verify_detached(signature.data(),
                                  reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                                  signed_bytes.size(),
                                  keys.public_key.data()) != 0)
    return std::unexpected(Error{core::ErrorKind::auth, "invalid QQ webhook signature"});
  return {};
}

async::Awaitable<core::Result<channel::Response>> accept_qq_webhook(QQWebhookRequest request,
                                                                    channel::Account account,
                                                                    std::string_view secret,
                                                                    std::chrono::sys_seconds now,
                                                                    hook::Bus& hooks,
                                                                    const permission::RuleSet& rules,
                                                                    const QQEnqueue& enqueue) {
  const auto cancelled = co_await asio::this_coro::cancellation_state;
  if (cancelled.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  if (account.id.empty())
    co_return std::unexpected(Error::invalid_argument("QQ account identity is required"));
  auto verified = verify_qq_webhook(request, secret, now);
  if (!verified)
    co_return std::unexpected(verified.error());
  try {
    const auto body = Json::parse(request.body);
    const auto op = body.at("op").get<int>();
    if (op == 13) {
      const auto plain = body.at("d").at("plain_token").get<std::string>();
      const auto timestamp = body.at("d").at("event_ts").get<std::string>();
      if (plain.empty() || plain.size() > 4096 || timestamp.empty() || timestamp.size() > 16)
        co_return std::unexpected(Error::parsing("invalid QQ webhook challenge"));
      Keys keys{secret};
      const auto signed_bytes = timestamp + plain;
      std::array<unsigned char, crypto_sign_BYTES> signature{};
      crypto_sign_detached(signature.data(),
                           nullptr,
                           reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                           signed_bytes.size(),
                           keys.private_key.data());
      std::array<char, crypto_sign_BYTES * 2 + 1> encoded{};
      sodium_bin2hex(encoded.data(), encoded.size(), signature.data(), signature.size());
      co_return response({{"plain_token", plain}, {"signature", encoded.data()}});
    }
    if (op == 1) {
      if (!body.at("d").is_number_unsigned())
        co_return std::unexpected(Error::parsing("invalid QQ heartbeat sequence"));
      co_return response({{"op", 11}, {"d", body.at("d")}});
    }
    if (op != 0)
      co_return std::unexpected(Error::parsing("unsupported QQ webhook operation"));
    auto message = channel::qq().decode(request.body, account);
    if (!message)
      co_return std::unexpected(message.error());
    if (!*message)
      co_return response({{"op", 12}, {"d", 0}});
    const auto input = Json{{"conversation", channel::conversation_key((**message).conversation)},
                            {"sender", (**message).sender},
                            {"event_id", (**message).event_id}}
                           .dump();
    const auto decision = permission::evaluate(rules, "QQInbox", input, {}, permission::Mode::strict);
    co_await hooks.publish_advisory(
        hook::Event::channel_action,
        hook::ChannelActionPayload{"qq", account.id, "QQInbox", decision.verdict == permission::Verdict::allow});
    if (decision.verdict != permission::Verdict::allow)
      co_return std::unexpected(Error::permission_denied("QQ inbox denied"));
    if (!enqueue)
      co_return std::unexpected(Error::invalid_argument("QQ durable enqueue port is required"));
    auto queued = co_await enqueue(std::move(**message));
    if (!queued && queued.error().kind() == core::ErrorKind::cancelled)
      co_return std::unexpected(Error::cancelled());
    co_return response({{"op", 12}, {"d", queued ? 0 : 1}});
  } catch (const Json::exception&) {
    co_return std::unexpected(Error::parsing("invalid QQ webhook payload"));
  } catch (...) {
    // Injected ports can carry credentials or private message text in exceptions.
  }
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::internal("QQ webhook intake failed"));
}
}  // namespace orangutan::bootstrap
