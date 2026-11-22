#include "../test-helpers/run_async.hpp"
#include <asio/awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

TEST_CASE("Channel HTTP binding rejects unsafe routes before looking up credentials") {
  orangutan::tests::run_async([](asio::io_context& io) -> orangutan::async::Awaitable<void> {
    orangutan::http::Client client{io.get_executor()};
    int lookups = 0;
    auto transport = orangutan::bootstrap::channel_http_transport(
        client,
        [&](orangutan::channel::Conversation) -> orangutan::async::Awaitable<orangutan::core::Result<std::string>> {
          ++lookups;
          co_return std::unexpected(orangutan::core::Error::config("SECRET"));
        });
    orangutan::channel::Conversation conversation{orangutan::channel::Platform::telegram,
                                                  "bot",
                                                  orangutan::channel::ChatKind::direct,
                                                  "1",
                                                  {}};
    for (const std::string path : {"//evil.test", "/sendMessage?token=bad", "/deleteWebhook", "/../sendMessage"}) {
      auto result = co_await transport(conversation, {"POST", path, "{}"});
      REQUIRE_FALSE(result);
      CHECK(lookups == 0);
    }
    auto result = co_await transport(conversation, {"POST", "/sendMessage", "{}"});
    REQUIRE_FALSE(result);
    CHECK(lookups == 1);
    CHECK(result.error().kind() == orangutan::core::ErrorKind::auth);
    CHECK_FALSE(result.error().message().contains("SECRET"));
    for (const std::string path : {"/setMessageReaction", "/sendMessageDraft", "/getFile"}) {
      auto supported = co_await transport(conversation, {"POST", path, "{}"});
      REQUIRE_FALSE(supported);
      CHECK(supported.error().kind() == orangutan::core::ErrorKind::auth);
    }
    CHECK(lookups == 4);
    auto cancelled_transport = orangutan::bootstrap::channel_http_transport(
        client,
        [](orangutan::channel::Conversation) -> orangutan::async::Awaitable<orangutan::core::Result<std::string>> {
          co_return std::unexpected(orangutan::core::Error::cancelled());
        });
    auto cancelled = co_await cancelled_transport(conversation, {"POST", "/sendMessage", "{}"});
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().kind() == orangutan::core::ErrorKind::cancelled);
  });
}

TEST_CASE("QQ and Feishu bindings only authorize implemented message and reaction routes") {
  orangutan::tests::run_async([](asio::io_context& io) -> orangutan::async::Awaitable<void> {
    using namespace orangutan;
    http::Client client{io.get_executor()};
    int lookups = 0;
    auto transport =
        bootstrap::channel_http_transport(client,
                                          [&](channel::Conversation) -> async::Awaitable<core::Result<std::string>> {
                                            ++lookups;
                                            co_return std::unexpected(core::Error::config("unavailable"));
                                          });
    channel::Conversation target{channel::Platform::qq, "app", channel::ChatKind::direct, "owner", {}};
    for (const auto* path : {"/v2/users/owner",
                             "/v2/users/owner/files",
                             "/v2/users/a/b/messages",
                             "/v2/users/%2e%2e/messages",
                             "/v2/users/a%2fb/messages"}) {
      auto rejected = co_await transport(target, {"POST", path, "{}"});
      REQUIRE_FALSE(rejected);
      CHECK(lookups == 0);
    }
    auto deleted = co_await transport(target, {"DELETE", "/v2/users/owner/messages", "{}"});
    REQUIRE_FALSE(deleted);
    CHECK(lookups == 0);
    auto valid = co_await transport(target, {"POST", "/v2/users/owner/messages", "{}"});
    REQUIRE_FALSE(valid);
    CHECK(lookups == 1);
    target.platform = channel::Platform::feishu;
    for (const auto* path : {"/open-apis/im/v1/messages/id",
                             "/open-apis/im/v1/messages/id/other",
                             "/open-apis/im/v1/messages/id/reactions/r/extra"}) {
      auto rejected = co_await transport(target, {"DELETE", path, "{}"});
      REQUIRE_FALSE(rejected);
      CHECK(lookups == 1);
    }
    auto reaction = co_await transport(target, {"DELETE", "/open-apis/im/v1/messages/id/reactions/r", "{}"});
    REQUIRE_FALSE(reaction);
    CHECK(lookups == 2);
  });
}
