#include <oran/bootstrap/qq_connect.hpp>

#include <algorithm>
#include <array>
#include <asio/this_coro.hpp>
#include <format>
#include <nlohmann/json.hpp>
#include <oran/async/sleep.hpp>
#include <oran/hook/bus.hpp>
#include <sodium.h>

namespace orangutan::bootstrap {
namespace {
using core::Error;
using core::Result;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

struct BindKey {
  std::array<unsigned char, crypto_aead_aes256gcm_KEYBYTES> bytes{};
  ~BindKey() {
    sodium_memzero(bytes.data(), bytes.size());
  }
};

std::string encode_component(std::string_view value) {
  std::string encoded;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~')
      encoded += static_cast<char>(c);
    else
      encoded += std::format("%{:02X}", c);
  }
  return encoded;
}

Result<std::string> decrypt_secret(std::string_view encoded, const BindKey& key) {
  if (encoded.empty() || encoded.size() > 8192)
    return std::unexpected(Error::parsing("invalid QQ encrypted credential"));
  std::array<unsigned char, 6144> cipher{};
  std::size_t size{};
  const char* end{};
  if (sodium_base642bin(cipher.data(),
                        cipher.size(),
                        encoded.data(),
                        encoded.size(),
                        nullptr,
                        &size,
                        &end,
                        sodium_base64_VARIANT_ORIGINAL) != 0 ||
      end != encoded.data() + encoded.size() || size <= crypto_aead_aes256gcm_NPUBBYTES + crypto_aead_aes256gcm_ABYTES)
    return std::unexpected(Error::parsing("invalid QQ encrypted credential"));
  const auto length = size - crypto_aead_aes256gcm_NPUBBYTES - crypto_aead_aes256gcm_ABYTES;
  if (length > 4096)
    return std::unexpected(Error::parsing("invalid QQ credential length"));
  std::string secret(length, '\0');
  if (crypto_aead_aes256gcm_decrypt(reinterpret_cast<unsigned char*>(secret.data()),
                                    nullptr,
                                    nullptr,
                                    cipher.data() + crypto_aead_aes256gcm_NPUBBYTES,
                                    size - crypto_aead_aes256gcm_NPUBBYTES,
                                    nullptr,
                                    0,
                                    cipher.data(),
                                    key.bytes.data()) != 0 ||
      !std::ranges::all_of(secret, [](unsigned char c) { return c > 32 && c < 127; })) {
    sodium_memzero(secret.data(), secret.size());
    return std::unexpected(Error{core::ErrorKind::auth, "QQ credential authentication failed"});
  }
  return secret;
}

async::Awaitable<Result<Json>>
request(QQConnectOptions& options, std::string_view path, Json body, Clock::time_point deadline) {
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const std::array required{core::Capability::egress_http};
  auto decision = permission::evaluate(options.rules, "QQBind", path, required, permission::Mode::strict);
  co_await options.hooks->publish_advisory(
      hook::Event::channel_action,
      hook::ChannelActionPayload{"qq", "onboarding", "QQBind", decision.verdict == permission::Verdict::allow});
  if (decision.verdict != permission::Verdict::allow)
    co_return std::unexpected(Error::permission_denied("QQ binding denied"));
  if (cancellation.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(Error::cancelled());
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
  if (remaining <= std::chrono::milliseconds::zero())
    co_return std::unexpected(Error::timeout(options.timeout));
  http::BodyRequest input;
  input.method = "POST";
  input.url = "https://q.qq.com" + std::string{path};
  input.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json"}};
  input.body = body.dump();
  input.timeout = std::min(std::chrono::milliseconds{10000}, remaining);
  input.max_bytes = 16384;
  auto response = co_await options.send(std::move(input));
  if (!response)
    co_return std::unexpected(Error{response.error().kind(), "QQ binding request failed"});
  if (response->status_code != 200 || response->body.size() > 16384)
    co_return std::unexpected(Error{core::ErrorKind::auth, "QQ binding request rejected"});
  auto parsed = Json::parse(response->body);
  if (!parsed.at("retcode").is_number_integer() || parsed.at("retcode") != 0 || !parsed.at("data").is_object())
    co_return std::unexpected(Error{core::ErrorKind::auth, "QQ binding request rejected"});
  co_return std::move(parsed["data"]);
}

async::Awaitable<Result<QQCredentials>> connect_impl(QQConnectOptions& options) {
  if (!options.send || !options.hooks || !options.display || options.timeout < std::chrono::milliseconds{1} ||
      options.timeout > std::chrono::minutes{10} || options.poll_interval < std::chrono::milliseconds{1} ||
      options.poll_interval > std::chrono::seconds{10})
    co_return std::unexpected(Error::invalid_argument("invalid QQ binding options"));
  if (sodium_init() < 0 || !crypto_aead_aes256gcm_is_available())
    co_return std::unexpected(Error::config("QQ binding requires hardware AES-GCM support"));
  const auto executor = co_await asio::this_coro::executor;
  const auto deadline = Clock::now() + options.timeout;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    BindKey key;
    randombytes_buf(key.bytes.data(), key.bytes.size());
    std::array<char, 45> encoded{};
    sodium_bin2base64(encoded.data(),
                      encoded.size(),
                      key.bytes.data(),
                      key.bytes.size(),
                      sodium_base64_VARIANT_ORIGINAL);
    Json body{{"key", encoded.data()}};
    sodium_memzero(encoded.data(), encoded.size());
    auto task = co_await request(options, "/lite/create_bind_task", std::move(body), deadline);
    if (!task)
      co_return std::unexpected(task.error());
    const auto cancellation = co_await asio::this_coro::cancellation_state;
    if (cancellation.cancelled() != asio::cancellation_type::none)
      co_return std::unexpected(Error::cancelled());
    if (Clock::now() >= deadline)
      co_return std::unexpected(Error::timeout(options.timeout));
    const auto id = task->at("task_id").get<std::string>();
    if (id.empty() || id.size() > 256)
      co_return std::unexpected(Error::parsing("invalid QQ binding task"));
    auto displayed = co_await options.display(
        "https://q.qq.com/qqbot/openclaw/connect.html?task_id=" + encode_component(id) + "&source=orangutan&_wv=2");
    if (!displayed)
      co_return std::unexpected(Error{displayed.error().kind(), "QQ QR display failed"});
    for (;;) {
      Json poll{{"task_id", id}};
      auto result = co_await request(options, "/lite/poll_bind_result", std::move(poll), deadline);
      if (!result)
        co_return std::unexpected(result.error());
      const auto cancellation = co_await asio::this_coro::cancellation_state;
      if (cancellation.cancelled() != asio::cancellation_type::none)
        co_return std::unexpected(Error::cancelled());
      if (Clock::now() >= deadline)
        co_return std::unexpected(Error::timeout(options.timeout));
      const auto& status = result->at("status");
      if (!status.is_number_integer() || status < 0 || status > 3)
        co_return std::unexpected(Error::parsing("invalid QQ binding status"));
      if (status == 3)
        break;
      if (status == 2) {
        const auto& app = result->at("bot_appid");
        QQCredentials credentials;
        credentials.app_id = app.is_string() ? app.get<std::string>() : app.is_number_integer() ? app.dump() : "";
        credentials.user_openid = result->value("user_openid", std::string{});
        if (credentials.app_id.empty() || credentials.app_id.size() > 64 ||
            !std::ranges::all_of(credentials.app_id, [](char c) { return c >= '0' && c <= '9'; }) ||
            credentials.user_openid.size() > 256 ||
            !std::ranges::all_of(credentials.user_openid, [](unsigned char c) { return c > 32 && c < 127; }))
          co_return std::unexpected(Error::parsing("invalid QQ bound identity"));
        auto secret = decrypt_secret(result->at("bot_encrypt_secret").get<std::string>(), key);
        if (!secret)
          co_return std::unexpected(secret.error());
        credentials.app_secret = std::move(*secret);
        co_return credentials;
      }
      const auto delay = std::min(options.poll_interval,
                                  std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
      auto slept = co_await async::sleep_for(executor, delay);
      if (!slept)
        co_return std::unexpected(slept.error());
    }
  }
  co_return std::unexpected(Error{core::ErrorKind::timeout, "QQ binding QR refresh limit reached"});
}
}  // namespace

async::Awaitable<core::Result<QQCredentials>> connect_qq(QQConnectOptions& options) {
  try {
    co_return co_await connect_impl(options);
  } catch (...) {
    // JSON, HTTP and display exceptions can contain credentials; never copy them.
  }
  const auto cancellation = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancellation.cancelled() != asio::cancellation_type::none
                                ? Error::cancelled()
                                : Error::parsing("QQ binding failed"));
}
}  // namespace orangutan::bootstrap
