#include "../../apps/qq/login.hpp"
#include "../test-helpers/run_async.hpp"

#include <array>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <oran/async/channel.hpp>
#include <oran/async/sleep.hpp>
#include <oran/hook/bus.hpp>
#include <sodium.h>

namespace {
using namespace orangutan;
using core::Result;
using Json = nlohmann::json;

struct Server {
  std::array<unsigned char, 32> key{};
  int creates{}, polls{}, displays{};
  std::vector<int> states{0, 1, 2};
  bool tampered{}, malformed{};
  std::string encrypted() const {
    constexpr std::string_view secret = "test-bot-secret";
    std::array<unsigned char, 12 + secret.size() + 16> bytes{};
    // Fixture follows Node's nonce(12) + ciphertext + GCM tag(16) wire layout.
    REQUIRE(crypto_aead_aes256gcm_encrypt(bytes.data() + 12,
                                          nullptr,
                                          reinterpret_cast<const unsigned char*>(secret.data()),
                                          secret.size(),
                                          nullptr,
                                          0,
                                          nullptr,
                                          bytes.data(),
                                          key.data()) == 0);
    if (tampered)
      bytes.back() ^= 1;
    std::array<char, 128> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(), sodium_base64_VARIANT_ORIGINAL);
    return encoded.data();
  }
  async::Awaitable<Result<http::BodyResponse>> send(http::BodyRequest input) {
    CHECK(input.method == "POST");
    CHECK(input.timeout <= std::chrono::seconds{10});
    CHECK(input.max_bytes == 16384);
    const auto body = Json::parse(input.body);
    Json data;
    if (input.url == "https://q.qq.com/lite/create_bind_task") {
      ++creates;
      const auto encoded = body.at("key").get<std::string>();
      std::size_t length{};
      REQUIRE(sodium_base642bin(key.data(),
                                key.size(),
                                encoded.data(),
                                encoded.size(),
                                nullptr,
                                &length,
                                nullptr,
                                sodium_base64_VARIANT_ORIGINAL) == 0);
      REQUIRE(length == key.size());
      data = {{"task_id", "task&x=1"}};
    } else {
      CHECK(input.url == "https://q.qq.com/lite/poll_bind_result");
      CHECK(body == Json{{"task_id", "task&x=1"}});
      CHECK(displays == creates);
      const auto status = states.at(std::min(static_cast<std::size_t>(polls++), states.size() - 1));
      data = {{"status", status}};
      if (status == 2) {
        data["bot_appid"] = 123456;
        data["bot_encrypt_secret"] = malformed ? "not-base64" : encrypted();
        data["user_openid"] = "owner-123";
      }
    }
    co_return http::BodyResponse{200, {}, Json{{"retcode", 0}, {"data", data}}.dump()};
  }
  bootstrap::QQConnectOptions options(hook::Bus& hooks) {
    bootstrap::QQConnectOptions options;
    options.send = [this](http::BodyRequest input) {
      return send(std::move(input));
    };
    options.hooks = &hooks;
    for (const auto* name : {"QQBind", "QQToken", "QQCredentialRead", "QQCredentialWrite"})
      options.rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = name});
    options.poll_interval = std::chrono::milliseconds{1};
    options.display = [this](std::string url) -> async::Awaitable<Result<void>> {
      ++displays;
      CHECK(url == "https://q.qq.com/qqbot/openclaw/connect.html?task_id=task%26x%3D1&source=orangutan&_wv=2");
      co_return Result<void>{};
    };
    return options;
  }
};

struct Temporary {
  std::filesystem::path path;
  ~Temporary() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
}  // namespace

TEST_CASE("QQ binding handles pending states and decrypts an authenticated bot identity", "[qq-connect]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    Server server;
    auto options = server.options(hooks);
    auto result = co_await bootstrap::connect_qq(options);
    REQUIRE(result);
    CHECK(result->app_id == "123456");
    CHECK(result->app_secret == "test-bot-secret");
    CHECK(result->user_openid == "owner-123");
    CHECK(server.creates == 1);
    CHECK(server.polls == 3);
  });
}

TEST_CASE("QQ binding rejects tampered ciphertext and malformed states without leaking responses", "[qq-connect]") {
  for (int scenario = 0; scenario < 5; ++scenario) {
    tests::run_async([scenario](asio::io_context&) -> async::Awaitable<void> {
      hook::Bus hooks;
      Server server;
      server.states = {2};
      server.tampered = scenario == 0;
      server.malformed = scenario == 1;
      if (scenario == 2)
        server.states = {4};
      auto options = server.options(hooks);
      if (scenario == 3)
        options.send = [](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
          co_return http::BodyResponse{200, {}, R"({"retcode":0,"data":{"task_id":false},"secret":"DO-NOT-LOG"})"};
        };
      if (scenario == 4)
        options.send = [](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
          throw std::runtime_error("DO-NOT-LOG");
          co_return http::BodyResponse{};
        };
      auto result = co_await bootstrap::connect_qq(options);
      REQUIRE_FALSE(result);
      CHECK_FALSE(result.error().message().contains("DO-NOT-LOG"));
      CHECK(result.error().context().empty());
    });
  }
}

TEST_CASE("QQ binding denial precedes network and display effects", "[qq-connect]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    Server server;
    auto options = server.options(hooks);
    options.rules.clear();
    int observed = 0;
    hooks.subscribe({.id = "observe",
                     .observe = [&](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<void> {
                       const auto& action = std::get<hook::ChannelActionPayload>(*payload);
                       CHECK(action.operation == "QQBind");
                       CHECK_FALSE(action.allowed);
                       ++observed;
                       co_return;
                     }},
                    {hook::Event::channel_action});
    auto result = co_await bootstrap::connect_qq(options);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind() == core::ErrorKind::permission_denied);
    CHECK(observed == 1);
    CHECK(server.creates == 0);
    CHECK(server.displays == 0);
  });
}

TEST_CASE("QQ binding bounds QR renewal and total waiting", "[qq-connect]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    Server server;
    server.states = {3};
    auto options = server.options(hooks);
    auto expired = co_await bootstrap::connect_qq(options);
    REQUIRE_FALSE(expired);
    CHECK(expired.error().kind() == core::ErrorKind::timeout);
    CHECK(server.creates == 3);
    CHECK(server.displays == 3);
    server.states = {1};
    options.timeout = std::chrono::milliseconds{5};
    auto timeout = co_await bootstrap::connect_qq(options);
    REQUIRE_FALSE(timeout);
    CHECK(timeout.error().kind() == core::ErrorKind::timeout);
  });
}

TEST_CASE("QQ binding cancellation joins its in-flight request", "[qq-connect]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    Server server;
    auto options = server.options(hooks);
    asio::cancellation_signal cancellation;
    async::Channel<bool> entered{io.get_executor(), 1}, finished{io.get_executor(), 1};
    bool released = false;
    options.send = [&](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
      REQUIRE(entered.try_send(true));
      auto waiting = co_await async::sleep_for(io.get_executor(), std::chrono::hours{1});
      released = true;
      REQUIRE_FALSE(waiting);
      co_return std::unexpected(waiting.error());
    };
    asio::co_spawn(io,
                   bootstrap::connect_qq(options),
                   asio::bind_cancellation_slot(cancellation.slot(),
                                                [&](std::exception_ptr error, Result<bootstrap::QQCredentials> result) {
                                                  CHECK_FALSE(error);
                                                  REQUIRE_FALSE(result);
                                                  CHECK(result.error().kind() == core::ErrorKind::cancelled);
                                                  CHECK(released);
                                                  REQUIRE(finished.try_send(true));
                                                }));
    auto ready = co_await entered.receive();
    REQUIRE(ready);
    cancellation.emit(asio::cancellation_type::all);
    auto done = co_await finished.receive();
    REQUIRE(done);
    CHECK(server.displays == 0);
  });
}

TEST_CASE("QQ login persists a private binding, protects it and probes without scanning", "[qq-connect]") {
  auto id = core::generate_turn_id();
  REQUIRE(id);
  Temporary temp{std::filesystem::temp_directory_path() / ("oran-qq-login-" + core::format_turn_id_hex(*id))};
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto directory = io::PrivateDirectory::open(temp.path.string());
    REQUIRE(directory);
    auto lock = directory->lock("login.lock");
    REQUIRE(lock);
    hook::Bus hooks;
    Server server;
    auto denied = server.options(hooks);
    denied.rules.clear();
    denied.rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "QQCredentialRead"});
    auto refused = co_await qq_login::run(false, std::move(denied), *directory, io.get_executor());
    REQUIRE_FALSE(refused);
    CHECK(server.creates == 0);
    CHECK_FALSE(std::filesystem::exists(temp.path / "qq-credentials.json"));
    auto result = co_await qq_login::run(false, server.options(hooks), *directory, io.get_executor());
    REQUIRE(result);
    CHECK(*result == "123456");
    const auto permissions = std::filesystem::status(temp.path / "qq-credentials.json").permissions();
    CHECK((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
    const auto original = directory->read("qq-credentials.json", 16384);
    REQUIRE(original);
    REQUIRE(*original);
    const auto saved = Json::parse(**original);
    CHECK(saved.at("app_secret") == "test-bot-secret");
    CHECK(saved.at("user_openid") == "owner-123");
    auto repeated = co_await qq_login::run(false, server.options(hooks), *directory, io.get_executor());
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().kind() == core::ErrorKind::conflict);
    CHECK(server.creates == 1);
    auto options = server.options(hooks);
    int tokens = 0;
    options.send = [&](http::BodyRequest input) -> async::Awaitable<Result<http::BodyResponse>> {
      ++tokens;
      CHECK(input.url == "https://bots.qq.com/app/getAppAccessToken");
      CHECK(Json::parse(input.body).at("clientSecret") == "test-bot-secret");
      co_return http::BodyResponse{200, {}, R"({"access_token":"test-access-token","expires_in":7200})"};
    };
    auto probe = co_await qq_login::run(true, std::move(options), *directory, io.get_executor());
    REQUIRE(probe);
    CHECK(tokens == 1);
    CHECK(server.displays == 1);
    const auto unchanged = directory->read("qq-credentials.json", 16384);
    REQUIRE(unchanged);
    CHECK(*unchanged == *original);
  });
}
