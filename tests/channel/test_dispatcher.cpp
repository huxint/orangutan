#include "../test-helpers/run_async.hpp"
#include <asio/awaitable.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <oran/channel.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/bus.hpp>

namespace channel = orangutan::channel;
namespace core = orangutan::core;
namespace async = orangutan::async;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
using namespace std::chrono_literals;

namespace {
channel::Message message(channel::Platform platform = channel::Platform::telegram) {
  return {{platform, "account", channel::ChatKind::direct, "123", {}}, "event", "456", "sender", "question"};
}
channel::DispatcherOptions options(hook::Bus& bus) {
  channel::DispatcherOptions result;
  result.hooks = &bus;
  result.mode = permission::Mode::permissive;
  result.typing_interval = 2ms;
  result.typing_ttl = 50ms;
  return result;
}
channel::Response telegram_success() {
  return {200, R"({"ok":true,"result":{"message_id":9}})", {}};
}
}  // namespace

TEST_CASE("Channel admission is fail closed and observed before effects") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    int observations = 0;
    bus.subscribe({.id = "capture",
                   .observe = [&](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<void> {
                     const auto& p = std::get<hook::ChannelActionPayload>(*payload);
                     CHECK_FALSE(p.allowed);
                     ++observations;
                     co_return;
                   }},
                  {hook::Event::channel_action});
    int sends = 0, turns = 0;
    auto opts = options(bus);
    opts.mode = permission::Mode::strict;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request) -> async::Awaitable<core::Result<channel::Response>> {
          ++sends;
          co_return telegram_success();
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          ++turns;
          co_return "answer";
        },
        std::move(opts));
    REQUIRE(dispatcher);
    auto result = co_await (*dispatcher)->handle(message());
    REQUIRE_FALSE(result);
    CHECK(result.error().kind() == core::ErrorKind::permission_denied);
    CHECK(observations == 1);
    CHECK(sends == 0);
    CHECK(turns == 0);
  });
}

TEST_CASE("Channel authorizes the host-rendered reply before transport") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    auto opts = options(bus);
    auto pattern = permission::InputPattern::compile("forbidden-destination");
    REQUIRE(pattern);
    opts.rules.push_back(
        {.verdict = permission::Verdict::deny, .tool_pattern = "ChannelSend", .input_pattern = std::move(*pattern)});
    opts.render_reply =
        [](const channel::Message& message, std::string_view text, std::size_t part) -> core::Result<channel::Request> {
      auto request = channel::telegram().reply(message, text, part);
      REQUIRE(request);
      request->body = R"({"text":"answer","entities":[{"url":"https://forbidden-destination.test"}]})";
      return request;
    };
    int replies = 0;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.path == "/sendMessage")
            ++replies;
          co_return telegram_success();
        },
        [](channel::Message) -> async::Awaitable<core::Result<std::string>> { co_return "answer"; },
        std::move(opts));
    REQUIRE(dispatcher);
    auto denied = co_await (*dispatcher)->handle(message());
    REQUIRE_FALSE(denied);
    CHECK(denied.error().kind() == core::ErrorKind::permission_denied);
    CHECK(replies == 0);
  });
}

TEST_CASE("Channel retries preserve generated replies and skip confirmed parts") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    int sends = 0, turns = 0;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.path == "/sendChatAction")
            co_return telegram_success();
          ++sends;
          if (sends == 2)
            co_return std::unexpected(core::Error::network("SECRET upstream"));
          co_return telegram_success();
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          ++turns;
          co_return std::string(8001, 'x');
        },
        options(bus));
    REQUIRE(dispatcher);
    auto first = co_await (*dispatcher)->handle(message());
    REQUIRE_FALSE(first);
    CHECK_FALSE(first.error().message().contains("SECRET"));
    auto pending = (*dispatcher)->pending(message());
    REQUIRE(pending);
    CHECK(pending->next_part == 1);
    CHECK(pending->parts.size() == 3);
    auto duplicate = co_await (*dispatcher)->handle(message());
    CHECK_FALSE(duplicate);
    CHECK(sends == 2);
    CHECK(turns == 1);
    auto recovered = co_await (*dispatcher)->resume(message(), 2);  // Host confirmed ambiguous part 1 was delivered.
    REQUIRE(recovered);
    CHECK(recovered->parts_sent == 3);
    CHECK(sends == 3);
    CHECK(turns == 1);
    auto completed = co_await (*dispatcher)->handle(message());
    REQUIRE(completed);
    CHECK(completed->duplicate);
    CHECK(sends == 3);
  });
}

TEST_CASE("Feishu late typing start is joined and removed on turn failure") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus bus;
    std::vector<std::string> calls;
    bool start_in_flight = false;
    asio::steady_timer started{io};
    started.expires_at(std::chrono::steady_clock::time_point::max());
    auto dispatcher = channel::Dispatcher::create(
        channel::feishu(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          calls.push_back(request.method);
          if (request.method == "POST") {
            start_in_flight = true;
            started.cancel();
            asio::steady_timer delayed{io, 5ms};
            co_await delayed.async_wait(asio::use_awaitable);
            co_return channel::Response{200, R"({"code":0,"data":{"reaction_id":"r1"}})", {}};
          }
          CHECK(request.path.ends_with("/reactions/r1"));
          co_return channel::Response{200, R"({"code":0,"data":{}})", {}};
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          asio::error_code error;
          if (!start_in_flight)
            co_await started.async_wait(asio::redirect_error(asio::use_awaitable, error));
          co_return std::unexpected(core::Error::upstream("turn failure"));
        },
        options(bus));
    REQUIRE(dispatcher);
    auto result = co_await (*dispatcher)->handle(message(channel::Platform::feishu));
    CHECK_FALSE(result);
    CHECK(calls == std::vector<std::string>{"POST", "DELETE"});
  });
}

TEST_CASE("Telegram renews typing without overlapping calls and stops after completion") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus bus;
    int typing = 0, replies = 0;
    bool active = false;
    asio::steady_timer renewed{io};
    renewed.expires_at(std::chrono::steady_clock::time_point::max());
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          CHECK_FALSE(active);
          active = true;
          if (request.path == "/sendChatAction") {
            if (++typing == 3)
              renewed.cancel();
            asio::steady_timer delay{io, 1ms};
            co_await delay.async_wait(asio::use_awaitable);
          } else
            ++replies;
          active = false;
          co_return telegram_success();
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          asio::error_code error;
          co_await renewed.async_wait(asio::redirect_error(asio::use_awaitable, error));
          co_return "answer";
        },
        options(bus));
    REQUIRE(dispatcher);
    auto result = co_await (*dispatcher)->handle(message());
    REQUIRE(result);
    CHECK(typing == 3);
    CHECK(replies == 1);
    CHECK_FALSE(active);
  });
}

TEST_CASE("Channel cancellation joins Feishu activity cleanup before returning") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus bus;
    asio::cancellation_signal cancel;
    asio::steady_timer waiting{io};
    waiting.expires_at(std::chrono::steady_clock::time_point::max());
    bool removed = false, finished = false;
    auto dispatcher = channel::Dispatcher::create(
        channel::feishu(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.method == "POST") {
            cancel.emit(asio::cancellation_type::terminal);
            asio::steady_timer late{io, 2ms};
            co_await late.async_wait(asio::use_awaitable);
            co_return channel::Response{200, R"({"code":0,"data":{"reaction_id":"owned"}})", {}};
          }
          removed = true;
          co_return channel::Response{200, R"({"code":0})", {}};
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          co_await waiting.async_wait(asio::use_awaitable);
          co_return "should not send";
        },
        options(bus));
    REQUIRE(dispatcher);
    asio::steady_timer done{io};
    done.expires_at(std::chrono::steady_clock::time_point::max());
    asio::co_spawn(io,
                   (*dispatcher)->handle(message(channel::Platform::feishu)),
                   asio::bind_cancellation_slot(cancel.slot(),
                                                [&](std::exception_ptr error, core::Result<channel::Delivery> result) {
                                                  CHECK_FALSE(error);
                                                  CHECK_FALSE(result);
                                                  if (!result)
                                                    CHECK(result.error().kind() == core::ErrorKind::cancelled);
                                                  CHECK(removed);
                                                  finished = true;
                                                  done.cancel();
                                                }));
    asio::error_code error;
    co_await done.async_wait(asio::redirect_error(asio::use_awaitable, error));
    CHECK(finished);
  });
}

TEST_CASE("Channel activity errors are advisory and rate limits stop renewals") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus bus;
    int typing = 0, replies = 0;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.path == "/sendChatAction") {
            ++typing;
            co_return channel::Response{429, "{}", 10s};
          }
          ++replies;
          co_return telegram_success();
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          asio::steady_timer delay{io, 10ms};
          co_await delay.async_wait(asio::use_awaitable);
          co_return "answer";
        },
        options(bus));
    REQUIRE(dispatcher);
    auto result = co_await (*dispatcher)->handle(message());
    REQUIRE(result);
    CHECK(typing == 1);
    CHECK(replies == 1);
    CHECK(result->activity_failures == 1);
  });
}

TEST_CASE("Pending replies apply backpressure and same conversations cannot overlap") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    auto opts = options(bus);
    opts.capacity = 1;
    channel::Dispatcher* active = nullptr;
    auto dispatcher = channel::Dispatcher::create(
        channel::qq(),
        [](channel::Conversation, channel::Request) -> async::Awaitable<core::Result<channel::Response>> {
          co_return std::unexpected(core::Error::network("unavailable"));
        },
        [&](channel::Message m) -> async::Awaitable<core::Result<std::string>> {
          auto nested = co_await active->handle(m);
          REQUIRE_FALSE(nested);
          CHECK(nested.error().kind() == core::ErrorKind::conflict);
          co_return "answer";
        },
        std::move(opts));
    REQUIRE(dispatcher);
    active = dispatcher->get();
    auto first = co_await active->handle(message(channel::Platform::qq));
    REQUIRE_FALSE(first);
    auto other = message(channel::Platform::qq);
    other.event_id = "another-event";
    other.conversation.account = "another-account";
    auto full = co_await active->handle(other);
    REQUIRE_FALSE(full);
    CHECK(full.error().kind() == core::ErrorKind::mailbox_overflowed);
    REQUIRE(active->pending(message(channel::Platform::qq)));
  });
}

TEST_CASE("Cancellation during the cleanup join cannot strand a late reaction") {
  orangutan::tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus bus;
    asio::cancellation_signal cancel;
    asio::steady_timer start{io}, done{io};
    start.expires_at(std::chrono::steady_clock::time_point::max());
    done.expires_at(std::chrono::steady_clock::time_point::max());
    bool started = false, removed = false, finished = false;
    auto dispatcher = channel::Dispatcher::create(
        channel::feishu(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.method == "POST") {
            started = true;
            start.cancel();
            asio::steady_timer delay{io, 5ms};
            co_await delay.async_wait(asio::use_awaitable);
            cancel.emit(asio::cancellation_type::terminal);
            delay.expires_after(2ms);
            co_await delay.async_wait(asio::use_awaitable);
            co_return channel::Response{200, R"({"code":0,"data":{"reaction_id":"late"}})", {}};
          }
          removed = true;
          co_return channel::Response{200, R"({"code":0})", {}};
        },
        [&](channel::Message) -> async::Awaitable<core::Result<std::string>> {
          asio::error_code error;
          if (!started)
            co_await start.async_wait(asio::redirect_error(asio::use_awaitable, error));
          co_return "saved answer";
        },
        options(bus));
    REQUIRE(dispatcher);
    asio::co_spawn(io,
                   (*dispatcher)->handle(message(channel::Platform::feishu)),
                   asio::bind_cancellation_slot(cancel.slot(),
                                                [&](std::exception_ptr error, core::Result<channel::Delivery> result) {
                                                  CHECK_FALSE(error);
                                                  REQUIRE_FALSE(result);
                                                  CHECK(result.error().kind() == core::ErrorKind::cancelled);
                                                  CHECK(removed);
                                                  finished = true;
                                                  done.cancel();
                                                }));
    asio::error_code error;
    co_await done.async_wait(asio::redirect_error(asio::use_awaitable, error));
    CHECK(finished);
    auto pending = (*dispatcher)->pending(message(channel::Platform::feishu));
    REQUIRE(pending);
    CHECK(pending->parts == std::vector<std::string>{"saved answer"});
  });
}

TEST_CASE("Denied outbound effects retain answers without sending and can be acknowledged") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    auto opts = options(bus);
    opts.capacity = 1;
    opts.rules.push_back(permission::Rule{.verdict = permission::Verdict::deny, .tool_pattern = "ChannelSend"});
    int sends = 0;
    auto dispatcher = channel::Dispatcher::create(
        channel::telegram(),
        [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
          if (request.path == "/sendMessage")
            ++sends;
          co_return telegram_success();
        },
        [](channel::Message) -> async::Awaitable<core::Result<std::string>> { co_return "saved"; },
        std::move(opts));
    REQUIRE(dispatcher);
    auto first = co_await (*dispatcher)->handle(message());
    REQUIRE_FALSE(first);
    CHECK(first.error().kind() == core::ErrorKind::permission_denied);
    CHECK(sends == 0);
    auto pending = (*dispatcher)->pending(message());
    REQUIRE(pending);
    CHECK(pending->parts == std::vector<std::string>{"saved"});
    REQUIRE((*dispatcher)->acknowledge_failure(message()));
    auto second = message();
    second.event_id = "second";
    auto next = co_await (*dispatcher)->handle(second);
    REQUIRE_FALSE(next);
    CHECK(next.error().kind() == core::ErrorKind::permission_denied);
    CHECK(sends == 0);
  });
}

TEST_CASE("Channel rejects invalid host-split parts before sending replies") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    for (const auto& part : {std::string{}, std::string(4001, 'x'), std::string(1, '\xff')}) {
      auto opts = options(bus);
      opts.split_reply = [&](std::string_view, std::size_t) -> core::Result<std::vector<std::string>> {
        return std::vector<std::string>{part};
      };
      int replies = 0;
      auto dispatcher = channel::Dispatcher::create(
          channel::telegram(),
          [&](channel::Conversation, channel::Request request) -> async::Awaitable<core::Result<channel::Response>> {
            if (request.path == "/sendMessage")
              ++replies;
            co_return telegram_success();
          },
          [](channel::Message) -> async::Awaitable<core::Result<std::string>> { co_return "answer"; },
          std::move(opts));
      REQUIRE(dispatcher);
      auto result = co_await (*dispatcher)->handle(message());
      CHECK_FALSE(result);
      CHECK(replies == 0);
    }
  });
}

TEST_CASE("QQ empty status acknowledgments cannot confirm message delivery") {
  orangutan::tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus bus;
    auto input = message(channel::Platform::qq);
    input.conversation.kind = channel::ChatKind::group;
    auto dispatcher = channel::Dispatcher::create(
        channel::qq(),
        [](channel::Conversation, channel::Request) -> async::Awaitable<core::Result<channel::Response>> {
          co_return channel::Response{200, "{}", {}};
        },
        [](channel::Message) -> async::Awaitable<core::Result<std::string>> { co_return "answer"; },
        options(bus));
    REQUIRE(dispatcher);
    auto result = co_await (*dispatcher)->handle(input);
    CHECK_FALSE(result);
    auto pending = (*dispatcher)->pending(input);
    REQUIRE(pending);
    CHECK(pending->next_part == 0);
  });
}
