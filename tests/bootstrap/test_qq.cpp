#include "../test-helpers/run_async.hpp"
#include <array>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <oran/async/channel.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/qq.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/bootstrap/session_status.hpp>
#include <oran/config/config.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/private_directory.hpp>
#include <ranges>
#include <sodium.h>

namespace {
using namespace orangutan;
using Json = nlohmann::json;
using core::Result;
constexpr std::string_view secret = "123456abcdef";
const std::chrono::sys_seconds now{std::chrono::seconds{1728981195}};

std::string sign(std::string_view timestamp, std::string_view body) {
  REQUIRE(sodium_init() >= 0);
  std::array<unsigned char, crypto_sign_SEEDBYTES> seed{};
  for (const auto i : std::views::iota(std::size_t{}, seed.size()))
    seed[i] = static_cast<unsigned char>(secret[i % secret.size()]);
  std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> pub{};
  std::array<unsigned char, crypto_sign_SECRETKEYBYTES> key{};
  crypto_sign_seed_keypair(pub.data(), key.data(), seed.data());
  const auto text = std::string{timestamp} + std::string{body};
  std::array<unsigned char, crypto_sign_BYTES> signature{};
  crypto_sign_detached(signature.data(),
                       nullptr,
                       reinterpret_cast<const unsigned char*>(text.data()),
                       text.size(),
                       key.data());
  std::array<char, crypto_sign_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), signature.data(), signature.size());
  return hex.data();
}
bootstrap::QQWebhookRequest request(Json body) {
  auto text = body.dump();
  return {"1728981195", sign("1728981195", text), std::move(text)};
}
Json event() {
  return {{"op", 0},
          {"t", "C2C_MESSAGE_CREATE"},
          {"d", {{"id", "message-1"}, {"content", "hello"}, {"author", {{"user_openid", "owner"}}}}}};
}
permission::RuleSet rules(std::string name) {
  permission::RuleSet result;
  result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = std::move(name)});
  return result;
}
channel::Conversation conversation() {
  return {channel::Platform::qq, "app", channel::ChatKind::direct, "owner", {}};
}
}  // namespace

TEST_CASE("QQ signatures match Tencent's published vector and reject tampering and stale requests", "[qq]") {
  bootstrap::QQWebhookRequest input{
      "1728981195",
      "e949b5b94ef4103df903fb031d1d16e358db3db83e79e117edd404c8508be3ce8a76d7bad1bed353194c126a1a5915b4ad8b5288c1191cc5"
      "3a12acffccd82004",
      R"({"id":"ROBOT1.0_veoihSEXDc8Q.g-6eLpNIa11bH8MisOjn-m-LKxCPntMk6exUXgcWCGpVO7L2QKTNZzjZzFFDSbiOFcqAPWyVA!!","content":"哦一下","timestamp":"2024-10-15T16:33:15+08:00","author":{"id":"675860273","user_openid":"675860273"}})"};
  CHECK(bootstrap::verify_qq_webhook(input, secret, now));
  CHECK_FALSE(bootstrap::verify_qq_webhook(input, secret, now + std::chrono::seconds{301}));
  input.body += ' ';
  CHECK_FALSE(bootstrap::verify_qq_webhook(input, secret, now));
  input = request(event());
  input.signature[0] = 'x';
  CHECK_FALSE(bootstrap::verify_qq_webhook(input, secret, now));
  input = request(event());
  input.body.assign(1024 * 1024 + 1, 'x');
  CHECK_FALSE(bootstrap::verify_qq_webhook(input, secret, now));
}

TEST_CASE("QQ challenges and heartbeats are authenticated and never enqueue messages", "[qq]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    permission::RuleSet denied;
    bootstrap::QQEnqueue unused;
    const auto challenge = request({{"op", 13}, {"d", {{"plain_token", "challenge"}, {"event_ts", "1728981194"}}}});
    auto result = co_await bootstrap::accept_qq_webhook(challenge, {"app", "bot"}, secret, now, hooks, denied, unused);
    REQUIRE(result);
    CHECK(Json::parse(result->body) ==
          Json{{"plain_token", "challenge"}, {"signature", sign("1728981194", "challenge")}});
    auto heartbeat = co_await bootstrap::accept_qq_webhook(request({{"op", 1}, {"d", 8}}),
                                                           {"app", "bot"},
                                                           secret,
                                                           now,
                                                           hooks,
                                                           denied,
                                                           unused);
    REQUIRE(heartbeat);
    CHECK(Json::parse(heartbeat->body) == Json{{"op", 11}, {"d", 8}});
  });
}

TEST_CASE("QQ dispatch ACK requires authorized completed enqueue and exposes no failed-port details", "[qq]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    int writes = 0;
    bool succeed = false;
    bootstrap::QQEnqueue enqueue = [&](channel::Message message) -> async::Awaitable<Result<void>> {
      ++writes;
      CHECK(message.sender == "owner");
      if (!succeed)
        co_return std::unexpected(core::Error::storage("PRIVATE"));
      co_return Result<void>{};
    };
    permission::RuleSet denied;
    auto result =
        co_await bootstrap::accept_qq_webhook(request(event()), {"app", "bot"}, secret, now, hooks, denied, enqueue);
    REQUIRE_FALSE(result);
    CHECK(writes == 0);
    auto allowed = rules("QQInbox");
    auto failed =
        co_await bootstrap::accept_qq_webhook(request(event()), {"app", "bot"}, secret, now, hooks, allowed, enqueue);
    REQUIRE(failed);
    CHECK(Json::parse(failed->body) == Json{{"op", 12}, {"d", 1}});
    CHECK_FALSE(failed->body.contains("PRIVATE"));
    succeed = true;
    auto accepted =
        co_await bootstrap::accept_qq_webhook(request(event()), {"app", "bot"}, secret, now, hooks, allowed, enqueue);
    REQUIRE(accepted);
    CHECK(writes == 2);
    CHECK(Json::parse(accepted->body) == Json{{"op", 12}, {"d", 0}});
  });
}

TEST_CASE("QQ token refresh is scoped, serialized, cached and renewed before expiry", "[qq]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    auto time = std::chrono::steady_clock::time_point{};
    async::Channel<bool> started{io.get_executor(), 1}, release{io.get_executor(), 1}, done{io.get_executor(), 2};
    int calls = 0;
    bootstrap::QQTokenOptions options{
        .app_id = "app",
        .app_secret = "SECRET",
        .executor = io.get_executor(),
        .send = [&](http::BodyRequest input) -> async::Awaitable<Result<http::BodyResponse>> {
          ++calls;
          CHECK(input.url == "https://bots.qq.com/app/getAppAccessToken");
          CHECK(input.max_bytes == 16384);
          CHECK(Json::parse(input.body).at("clientSecret") == "SECRET");
          if (calls == 1) {
            REQUIRE(started.try_send(true));
            auto released = co_await release.receive();
            REQUIRE(released);
          }
          co_return http::BodyResponse{200, {}, R"({"access_token":"TOKEN","expires_in":"60"})"};
        },
        .hooks = &hooks,
        .rules = rules("QQToken"),
        .now = [&] { return time; }};
    auto source = bootstrap::QQTokenSource::create(std::move(options));
    REQUIRE(source);
    auto complete = [&](std::exception_ptr error, Result<std::string> token) {
      CHECK_FALSE(error);
      REQUIRE(token);
      CHECK(*token == "TOKEN");
      REQUIRE(done.try_send(true));
    };
    asio::co_spawn(io, (*source)->get(conversation()), complete);
    auto ready = co_await started.receive();
    REQUIRE(ready);
    asio::co_spawn(io, (*source)->get(conversation()), complete);
    REQUIRE(release.try_send(true));
    auto first = co_await done.receive();
    auto second = co_await done.receive();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(calls == 1);
    auto foreign = conversation();
    foreign.account = "another-app";
    auto rejected = co_await (*source)->get(foreign);
    REQUIRE_FALSE(rejected);
    CHECK(calls == 1);
    time += std::chrono::seconds{55};
    auto refreshed = co_await (*source)->get(conversation());
    REQUIRE(refreshed);
    CHECK(calls == 2);
  });
}

TEST_CASE("QQ cancelled refresh waiters do not cancel the owner or leak the refresh permit", "[qq]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    asio::cancellation_signal cancellation;
    async::Channel<bool> started{io.get_executor(), 1}, release{io.get_executor(), 1}, done{io.get_executor(), 2};
    int calls = 0;
    auto source = bootstrap::QQTokenSource::create(
        {.app_id = "app",
         .app_secret = "SECRET",
         .executor = io.get_executor(),
         .send = [&](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
           ++calls;
           REQUIRE(started.try_send(true));
           auto released = co_await release.receive();
           REQUIRE(released);
           co_return http::BodyResponse{200, {}, R"({"access_token":"TOKEN","expires_in":7200})"};
         },
         .hooks = &hooks,
         .rules = rules("QQToken")});
    REQUIRE(source);
    asio::co_spawn(io, (*source)->get(conversation()), [&](std::exception_ptr error, Result<std::string> result) {
      CHECK_FALSE(error);
      REQUIRE(result);
      REQUIRE(done.try_send(true));
    });
    auto ready = co_await started.receive();
    REQUIRE(ready);
    asio::co_spawn(
        io,
        (*source)->get(conversation()),
        asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error, Result<std::string> result) {
          CHECK_FALSE(error);
          REQUIRE_FALSE(result);
          CHECK(result.error().kind() == core::ErrorKind::cancelled);
          REQUIRE(done.try_send(true));
        }));
    co_await asio::post(io, asio::use_awaitable);
    cancellation.emit(asio::cancellation_type::all);
    auto cancelled = co_await done.receive();
    REQUIRE(cancelled);
    REQUIRE(release.try_send(true));
    auto owner = co_await done.receive();
    REQUIRE(owner);
    auto cached = co_await (*source)->get(conversation());
    REQUIRE(cached);
    CHECK(calls == 1);
  });
}

TEST_CASE("QQ token permission and malformed responses cannot expose secrets", "[qq]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int calls = 0;
    bool valid = false;
    bootstrap::QQHttpSend send = [&](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
      ++calls;
      if (valid)
        co_return http::BodyResponse{200, {}, R"({"access_token":"RECOVERED","expires_in":7200})"};
      co_return http::BodyResponse{200, {}, R"({"access_token":"PRIVATE","expires_in":"PRIVATE"})"};
    };
    auto denied = bootstrap::QQTokenSource::create({.app_id = "app",
                                                    .app_secret = "PRIVATE",
                                                    .executor = io.get_executor(),
                                                    .send = send,
                                                    .hooks = &hooks,
                                                    .rules = {}});
    REQUIRE(denied);
    auto no = co_await (*denied)->get(conversation());
    REQUIRE_FALSE(no);
    CHECK(calls == 0);
    auto allowed = bootstrap::QQTokenSource::create({.app_id = "app",
                                                     .app_secret = "PRIVATE",
                                                     .executor = io.get_executor(),
                                                     .send = send,
                                                     .hooks = &hooks,
                                                     .rules = rules("QQToken")});
    REQUIRE(allowed);
    auto bad = co_await (*allowed)->get(conversation());
    REQUIRE_FALSE(bad);
    CHECK_FALSE(bad.error().message().contains("PRIVATE"));
    CHECK(calls == 1);
    valid = true;
    auto recovered = co_await (*allowed)->get(conversation());
    REQUIRE(recovered);
    CHECK(*recovered == "RECOVERED");
    CHECK(calls == 2);
  });
}

TEST_CASE("Authenticated QQ intake persists before ACK then runs and reopens a real agent session", "[qq]") {
  struct Temporary {
    std::filesystem::path path;
    ~Temporary() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };
  auto identity = core::generate_turn_id();
  REQUIRE(identity);
  Temporary temp{std::filesystem::temp_directory_path() / ("oran-qq-" + core::format_turn_id_hex(*identity))};
  std::filesystem::create_directories(temp.path / "workspace");
  auto inbox = io::PrivateDirectory::open((temp.path / "inbox").string());
  REQUIRE(inbox);
  tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    struct Model final : provider::System {
      mutable std::vector<provider::Request> requests;
      async::Awaitable<Result<provider::Response>>
      send(provider::Request input, provider::ModelTarget, provider::EventSink*) const override {
        requests.push_back(std::move(input));
        co_return provider::Response{.blocks = {core::TextContent{"answer"}},
                                     .stop_reason = core::StopReason::end_turn,
                                     .usage = {},
                                     .model_used = "fixture"};
      }
    } model;
    auto assembly = bootstrap::RuntimeAssembly::build((temp.path / "workspace").string(), io.get_executor());
    REQUIRE(assembly);
    config::Config config;
    std::optional<channel::Message> queued;
    auto allowed = rules("QQInbox");
    bootstrap::QQEnqueue enqueue = [&](channel::Message message) -> async::Awaitable<Result<void>> {
      auto saved = inbox->write("message.json", Json{{"id", message.event_id}, {"text", message.text}}.dump());
      if (!saved)
        co_return std::unexpected(saved.error());
      queued = std::move(message);
      co_return Result<void>{};
    };
    auto ack = co_await bootstrap::accept_qq_webhook(request(event()),
                                                     {"app", "bot"},
                                                     secret,
                                                     now,
                                                     assembly->hook_bus(),
                                                     allowed,
                                                     enqueue);
    REQUIRE(ack);
    REQUIRE(queued);
    auto saved = inbox->read("message.json", 4096);
    REQUIRE(saved);
    REQUIRE(*saved);
    CHECK(model.requests.empty());
    auto options = bootstrap::AgentSessionOptions{};
    options.executor = io.get_executor();
    options.blocking_executor = io.get_executor();
    options.assembly = &*assembly;
    options.config = &config;
    options.provider = &model;
    options.route.primary = {.profile = "fixture", .model = "fixture", .thinking_budget = {}, .cache = {}};
    options.session_id = *identity;
    options.agent_key = "qq";
    options.scope_key = channel::conversation_key(queued->conversation);
    options.mode = permission::Mode::permissive;
    auto session = bootstrap::AgentSession::create(options);
    REQUIRE(session);
    int sends = 0;
    channel::DispatcherOptions dispatch;
    dispatch.mode = permission::Mode::permissive;
    dispatch.hooks = &assembly->hook_bus();
    auto dispatcher = channel::Dispatcher::create(
        channel::qq(),
        [&](channel::Conversation, channel::Request outgoing) -> async::Awaitable<Result<channel::Response>> {
          CHECK(outgoing.path == "/v2/users/owner/messages");
          auto body = Json::parse(outgoing.body);
          if (body.at("msg_type") == 0) {
            ++sends;
            CHECK(body.at("content") == "answer");
          }
          co_return channel::Response{200, R"({"id":"receipt"})", {}};
        },
        [&](channel::Message message) -> async::Awaitable<Result<std::string>> {
          auto result = co_await (*session)->run_prompt({.prompt = std::move(message.text)});
          if (!result)
            co_return std::unexpected(result.error());
          co_return std::move(result->text);
        },
        std::move(dispatch));
    REQUIRE(dispatcher);
    auto first = co_await (*dispatcher)->handle(*queued);
    REQUIRE(first);
    auto duplicate = co_await (*dispatcher)->handle(*queued);
    REQUIRE(duplicate);
    CHECK(duplicate->duplicate);
    CHECK(sends == 1);
    session->reset();
    session = bootstrap::AgentSession::create(options);
    REQUIRE(session);
    queued->event_id = "message-2";
    queued->message_id = "message-2";
    queued->text = "continue";
    auto second = co_await (*dispatcher)->handle(*queued);
    REQUIRE(second);
    REQUIRE(model.requests.size() == 2);
    CHECK(model.requests.back().messages.size() == 3);
    auto status = co_await bootstrap::inspect_session(*assembly, *identity, "qq", io.get_executor());
    REQUIRE(status);
    CHECK(status->saved_messages == 4);
    auto other = co_await bootstrap::inspect_session(*assembly, *identity, "telegram", io.get_executor());
    REQUIRE(other);
    CHECK(other->saved_messages == 0);
  });
}
