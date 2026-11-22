#include "../../apps/telegram/format.hpp"
#include "../../apps/telegram/presentation.hpp"
#include "../test-helpers/run_async.hpp"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <oran/async/channel.hpp>
#include <oran/hook/bus.hpp>

namespace {
using namespace orangutan;
using core::Result;
using Json = nlohmann::json;
using namespace std::chrono_literals;

channel::Message message() {
  return {{channel::Platform::telegram, "bot", channel::ChatKind::direct, "42", {}}, "10", "9", "42", "hi"};
}
channel::Response success() {
  return {200, R"({"ok":true,"result":true})", {}};
}
permission::RuleSet status_rules() {
  permission::RuleSet rules;
  rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelStatus"});
  rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelDraft"});
  return rules;
}
channel::DispatcherOptions dispatch_options(hook::Bus& hooks) {
  channel::DispatcherOptions options;
  options.hooks = &hooks;
  options.mode = permission::Mode::permissive;
  return options;
}
channel::Transport final_transport(int& sends) {
  return [&sends](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
    if (request.path == "/sendMessage") {
      ++sends;
      co_return channel::Response{200, R"({"ok":true,"result":{"message_id":100}})", {}};
    }
    co_return success();
  };
}
}  // namespace

TEST_CASE("Telegram Markdown preserves Unicode entities and code across message boundaries", "[telegram-host]") {
  auto formatted = telegram_host::format_markdown("**😀中文** and [link](https://example.test)\n\n```cpp\n" +
                                                  std::string(9000, 'x') + "\n```");
  REQUIRE(formatted);
  CHECK(formatted->text.starts_with("😀中文 and link"));
  CHECK(formatted->entities.at(0) == Json{{"type", "bold"}, {"offset", 0}, {"length", 4}});
  auto parts = telegram_host::split_formatted(*formatted);
  REQUIRE(parts);
  REQUIRE(parts->size() == 3);
  std::string joined;
  for (const auto& part : *parts) {
    joined += part.text;
    CHECK(part.text.size() <= 4000);
    CHECK(std::ranges::any_of(part.entities, [](const Json& entity) {
      return entity.at("type") == "pre" && entity.at("language") == "cpp";
    }));
  }
  CHECK(joined == formatted->text);
  CHECK(parts->at(1).entities.at(0).at("offset") == 0);
  auto literal = telegram_host::format_markdown("<b>literal</b> [no](javascript:alert)\n\n> quoted\n\n1. one\n2. two");
  REQUIRE(literal);
  CHECK(literal->text.contains("<b>literal</b>"));
  CHECK(literal->text.contains("1. one"));
  CHECK(literal->text.contains("2. two"));
  CHECK_FALSE(
      std::ranges::any_of(literal->entities, [](const Json& entity) { return entity.at("type") == "text_link"; }));
  auto nested = telegram_host::format_markdown("**a `b` c**");
  REQUIRE(nested);
  CHECK(nested->text == "a b c");
  CHECK(nested->entities == Json::array({Json{{"type", "bold"}, {"offset", 0}, {"length", 2}},
                                         Json{{"type", "code"}, {"offset", 2}, {"length", 1}},
                                         Json{{"type", "bold"}, {"offset", 3}, {"length", 2}}}));
}

TEST_CASE("Telegram feedback serializes status and streamed drafts before final delivery", "[telegram-host]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    async::Channel<std::string> updates{io.get_executor(), 32};
    std::vector<std::string> reactions;
    int drafts = 0, sends = 0;
    telegram_host::Presentation presentation{
        io.get_executor(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          auto body = Json::parse(request.body);
          if (request.path == "/setMessageReaction") {
            auto emoji = body.at("reaction").at(0).at("emoji").get<std::string>();
            reactions.push_back(emoji);
            REQUIRE(updates.try_send(emoji));
          } else {
            REQUIRE(request.path == "/sendMessageDraft");
            CHECK(body.at("draft_id") == 9);
            CHECK(body.at("text") == "Hello 世界");
            CHECK(body.at("entities").at(0).at("type") == "bold");
            ++drafts;
            REQUIRE(updates.try_send("draft"));
          }
          co_return success();
        },
        hooks,
        status_rules(),
        1ms};
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        final_transport(sends),
        [&](channel::Message) -> async::Awaitable<Result<std::string>> {
          presentation.accepted();
          auto received = co_await updates.receive();
          REQUIRE(received);
          CHECK(*received == "👀");
          presentation.observe(hook::Event::provider_request);
          presentation.on_thinking_delta("private reasoning must never enter drafts");
          auto thinking = co_await updates.receive();
          REQUIRE(thinking);
          CHECK(*thinking == "🤔");
          presentation.on_tool_start("id", "FileRead");
          auto tool = co_await updates.receive();
          REQUIRE(tool);
          CHECK(*tool == "👨‍💻");
          presentation.observe(hook::Event::provider_request);
          presentation.on_text_delta("**Hello");
          presentation.on_text_delta(" 世界**");
          auto writing = co_await updates.receive();
          REQUIRE(writing);
          CHECK(*writing == "✍");
          auto draft = co_await updates.receive();
          REQUIRE(draft);
          CHECK(*draft == "draft");
          co_return "Hello 世界";
        },
        dispatch_options(hooks));
    REQUIRE(dispatcher);
    auto delivered = co_await presentation.deliver(**dispatcher, message());
    REQUIRE(delivered);
    CHECK(sends == 1);
    CHECK(drafts == 1);
    CHECK(reactions.back() == "👍");
    CHECK(delivered->activity_failures == 0);
  });
}

TEST_CASE("Telegram rejected or rate limited feedback never suppresses the answer", "[telegram-host]") {
  for (bool denied : {false, true}) {
    tests::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      hook::Bus hooks;
      int calls = 0, sends = 0;
      telegram_host::Presentation presentation{
          io.get_executor(),
          [&](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
            ++calls;
            co_return channel::Response{429, R"({"ok":false,"error_code":429,"parameters":{"retry_after":60}})", {}};
          },
          hooks,
          denied ? permission::RuleSet{} : status_rules(),
          1ms};
      auto dispatcher = channel::Dispatcher::create(
          channel::telegram(),
          final_transport(sends),
          [&](channel::Message) -> async::Awaitable<Result<std::string>> {
            presentation.accepted();
            presentation.on_text_delta("answer");
            co_await asio::post(io.get_executor(), asio::use_awaitable);
            co_return "answer";
          },
          dispatch_options(hooks));
      REQUIRE(dispatcher);
      auto delivered = co_await presentation.deliver(**dispatcher, message());
      REQUIRE(delivered);
      CHECK(sends == 1);
      CHECK(calls == (denied ? 0 : 1));
      CHECK(delivered->activity_failures > 0);
    });
  }
}

TEST_CASE("Telegram receive denial produces no reaction or draft", "[telegram-host]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    int calls = 0, sends = 0;
    telegram_host::Presentation presentation{
        io.get_executor(),
        [&](channel::Conversation, channel::Request) -> async::Awaitable<Result<channel::Response>> {
          ++calls;
          co_return success();
        },
        hooks,
        status_rules(),
        1ms};
    auto options = dispatch_options(hooks);
    options.mode = permission::Mode::strict;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        final_transport(sends),
        [&](channel::Message) -> async::Awaitable<Result<std::string>> {
          FAIL("denied turn ran");
          co_return "";
        },
        std::move(options));
    REQUIRE(dispatcher);
    auto denied = co_await presentation.deliver(**dispatcher, message());
    REQUIRE_FALSE(denied);
    CHECK(calls == 0);
    CHECK(sends == 0);
  });
}

TEST_CASE("Telegram cancellation joins the pending draft before the terminal reaction", "[telegram-host]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    asio::cancellation_signal cancellation;
    async::Channel<bool> draft_started{io.get_executor(), 1}, release_draft{io.get_executor(), 1},
        completed{io.get_executor(), 1};
    bool draft_finished = false;
    std::vector<std::string> reactions;
    int sends = 0;
    telegram_host::Presentation presentation{
        io.get_executor(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessageDraft") {
            REQUIRE(draft_started.try_send(true));
            auto released = co_await release_draft.receive();
            REQUIRE(released);
            draft_finished = true;
          } else {
            auto emoji = Json::parse(request.body).at("reaction").at(0).at("emoji").get<std::string>();
            if (emoji == "😱")
              CHECK(draft_finished);
            reactions.push_back(emoji);
          }
          co_return success();
        },
        hooks,
        status_rules(),
        1ms};
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        final_transport(sends),
        [&](channel::Message) -> async::Awaitable<Result<std::string>> {
          presentation.accepted();
          presentation.on_text_delta("partial");
          async::Channel<bool> never{io.get_executor(), 1};
          auto stopped = co_await never.receive();
          REQUIRE_FALSE(stopped);
          co_return std::unexpected(stopped.error());
        },
        dispatch_options(hooks));
    REQUIRE(dispatcher);
    asio::co_spawn(io,
                   presentation.deliver(**dispatcher, message()),
                   asio::bind_cancellation_slot(cancellation.slot(),
                                                [&](std::exception_ptr exception, Result<channel::Delivery> result) {
                                                  CHECK_FALSE(exception);
                                                  REQUIRE_FALSE(result);
                                                  CHECK(result.error().kind() == core::ErrorKind::cancelled);
                                                  REQUIRE(completed.try_send(true));
                                                }));
    auto started = co_await draft_started.receive();
    REQUIRE(started);
    cancellation.emit(asio::cancellation_type::all);
    REQUIRE(release_draft.try_send(true));
    auto done = co_await completed.receive();
    REQUIRE(done);
    CHECK(draft_finished);
    CHECK(reactions.back() == "😱");
    CHECK(sends == 0);
  });
}

TEST_CASE("Telegram final reply waits for the in-flight draft and never revives its preview", "[telegram-host]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    async::Channel<bool> draft_started{io.get_executor(), 1}, draining{io.get_executor(), 1},
        release_draft{io.get_executor(), 1}, completed{io.get_executor(), 1};
    bool draft_finished = false;
    int final_sends = 0, drafts = 0;
    telegram_host::Presentation presentation{
        io.get_executor(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessageDraft") {
            ++drafts;
            REQUIRE(draft_started.try_send(true));
            auto released = co_await release_draft.receive();
            REQUIRE(released);
            CHECK(final_sends == 0);
            draft_finished = true;
          }
          co_return success();
        },
        hooks,
        status_rules(),
        1ms};
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<Result<channel::Response>> {
          if (request.path == "/sendMessage") {
            CHECK(draft_finished);
            ++final_sends;
            co_return channel::Response{200, R"({"ok":true,"result":{"message_id":100}})", {}};
          }
          co_return success();
        },
        [&](channel::Message) -> async::Awaitable<Result<std::string>> {
          presentation.accepted();
          presentation.on_text_delta("answer");
          auto started = co_await draft_started.receive();
          REQUIRE(started);
          REQUIRE(draining.try_send(true));
          auto drained = co_await presentation.prepare_final();
          REQUIRE(drained);
          co_return "answer";
        },
        dispatch_options(hooks));
    REQUIRE(dispatcher);
    asio::co_spawn(io,
                   presentation.deliver(**dispatcher, message()),
                   [&](std::exception_ptr exception, Result<channel::Delivery> result) {
                     CHECK_FALSE(exception);
                     REQUIRE(result);
                     REQUIRE(completed.try_send(true));
                   });
    auto closing = co_await draining.receive();
    REQUIRE(closing);
    CHECK(final_sends == 0);
    REQUIRE(release_draft.try_send(true));
    auto done = co_await completed.receive();
    REQUIRE(done);
    CHECK(final_sends == 1);
    CHECK(drafts == 1);
  });
}
