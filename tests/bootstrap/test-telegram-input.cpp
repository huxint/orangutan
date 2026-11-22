#include "../../apps/telegram/input.hpp"
#include "../test-helpers/run_async.hpp"

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <nlohmann/json.hpp>
#include <oran/async/channel.hpp>
#include <oran/hook/bus.hpp>

namespace {
using namespace orangutan;
using core::Result;
using Json = nlohmann::json;
channel::Message image_message() {
  return {{channel::Platform::telegram, "bot", channel::ChatKind::direct, "42", {}},
          "1",
          "2",
          "42",
          "caption",
          channel::ImageAttachment{"file-id", "image/png", 12}};
}
permission::RuleSet image_rules() {
  permission::RuleSet rules;
  rules.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelAttachment"});
  return rules;
}
}  // namespace

TEST_CASE("Telegram image fetch checks authority and preserves inline bytes", "[telegram-host][image]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    int metadata = 0, downloads = 0;
    channel::Transport transport = [&](channel::Conversation,
                                       channel::Request request) -> async::Awaitable<Result<channel::Response>> {
      ++metadata;
      CHECK(request.path == "/getFile");
      CHECK(Json::parse(request.body).at("file_id") == "file-id");
      co_return channel::Response{200, R"({"ok":true,"result":{"file_path":"photos/file.png","file_size":12}})", {}};
    };
    telegram_host::DownloadImage download = [&](std::string path) -> async::Awaitable<Result<std::string>> {
      ++downloads;
      CHECK(path == "photos/file.png");
      co_return std::string{"\x89PNG\r\n\x1a\n"} + "test";
    };
    const auto message = image_message();
    permission::RuleSet denied;
    auto rejected = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, denied);
    REQUIRE_FALSE(rejected);
    CHECK(metadata == 0);
    CHECK(downloads == 0);
    auto rules = image_rules();
    auto loaded = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE(loaded);
    CHECK(loaded->media_type == "image/png");
    CHECK(loaded->data_base64 == "iVBORw0KGgp0ZXN0");
    CHECK(metadata == 1);
    CHECK(downloads == 1);
  });
}

TEST_CASE("Telegram image limits and unsafe paths fail before downloading", "[telegram-host][image]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    int metadata = 0, downloads = 0;
    std::string path = "https://untrusted.test/file.png";
    channel::Transport transport = [&](channel::Conversation,
                                       channel::Request) -> async::Awaitable<Result<channel::Response>> {
      ++metadata;
      co_return channel::Response{200, Json{{"ok", true}, {"result", {{"file_path", path}}}}.dump(), {}};
    };
    telegram_host::DownloadImage download = [&](std::string) -> async::Awaitable<Result<std::string>> {
      ++downloads;
      co_return std::string(telegram_host::image_max_bytes + 1, 'x');
    };
    auto message = image_message();
    auto rules = image_rules();
    message.image->file_size = telegram_host::image_max_bytes + 1;
    auto large = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(large);
    CHECK(metadata == 0);
    message.image->file_size = 0;
    auto unsafe = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(unsafe);
    CHECK(downloads == 0);
    path = "photos/image.png";
    auto oversized = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(oversized);
    CHECK(downloads == 1);
  });
}

TEST_CASE("Telegram image download requires its own authorization after metadata", "[telegram-host][image]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    int metadata = 0, downloads = 0;
    channel::Transport transport = [&](channel::Conversation,
                                       channel::Request) -> async::Awaitable<Result<channel::Response>> {
      ++metadata;
      co_return channel::Response{200, R"({"ok":true,"result":{"file_path":"photos/file.png"}})", {}};
    };
    telegram_host::DownloadImage download = [&](std::string) -> async::Awaitable<Result<std::string>> {
      ++downloads;
      co_return "unused";
    };
    auto pattern = permission::InputPattern::compile("getFile");
    REQUIRE(pattern);
    permission::RuleSet rules;
    rules.push_back({.verdict = permission::Verdict::allow,
                     .tool_pattern = "ChannelAttachment",
                     .input_pattern = std::move(*pattern)});
    const auto message = image_message();
    auto result = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(result);
    CHECK(result.error().kind() == core::ErrorKind::permission_denied);
    CHECK(metadata == 1);
    CHECK(downloads == 0);
  });
}

TEST_CASE("Telegram images use file signatures and redact transport failures", "[telegram-host][image]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    channel::Transport transport = [](channel::Conversation,
                                      channel::Request) -> async::Awaitable<Result<channel::Response>> {
      co_return channel::Response{200, R"({"ok":true,"result":{"file_path":"documents/file"}})", {}};
    };
    std::string bytes;
    bool fail = false;
    telegram_host::DownloadImage download = [&](std::string) -> async::Awaitable<Result<std::string>> {
      if (fail)
        co_return std::unexpected(core::Error::network("credential-bearing URL"));
      co_return bytes;
    };
    const auto message = image_message();
    const auto rules = image_rules();
    for (const auto& [signature, media] :
         std::vector<std::pair<std::string, std::string>>{{"\xff\xd8\xff", "image/jpeg"},
                                                          {"GIF89a", "image/gif"},
                                                          {"RIFF1234WEBP", "image/webp"}}) {
      bytes = signature;
      auto result = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
      REQUIRE(result);
      CHECK(result->media_type == media);
      CHECK_FALSE(result->data_base64.empty());
    }
    bytes = "<html>error response</html>";
    auto invalid = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().kind() == core::ErrorKind::invalid_argument);
    fail = true;
    auto failed = co_await telegram_host::load_image(message, *message.image, transport, download, hooks, rules);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().kind() == core::ErrorKind::network);
    CHECK_FALSE(failed.error().message().contains("credential-bearing"));
  });
}

TEST_CASE("Telegram image cancellation finishes the download before returning", "[telegram-host][image]") {
  tests::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    hook::Bus hooks;
    asio::cancellation_signal cancellation;
    async::Channel<bool> started{io.get_executor(), 1}, completed{io.get_executor(), 1}, never{io.get_executor(), 1};
    bool finished = false;
    channel::Transport transport = [](channel::Conversation,
                                      channel::Request) -> async::Awaitable<Result<channel::Response>> {
      co_return channel::Response{200, R"({"ok":true,"result":{"file_path":"photos/file.png"}})", {}};
    };
    telegram_host::DownloadImage download = [&](std::string) -> async::Awaitable<Result<std::string>> {
      REQUIRE(started.try_send(true));
      auto stopped = co_await never.receive();
      REQUIRE_FALSE(stopped);
      finished = true;
      co_return std::unexpected(stopped.error());
    };
    const auto message = image_message();
    const auto rules = image_rules();
    asio::co_spawn(io,
                   telegram_host::load_image(message, *message.image, transport, download, hooks, rules),
                   asio::bind_cancellation_slot(cancellation.slot(),
                                                [&](std::exception_ptr exception, Result<core::ImageContent> result) {
                                                  CHECK_FALSE(exception);
                                                  REQUIRE_FALSE(result);
                                                  CHECK(result.error().kind() == core::ErrorKind::cancelled);
                                                  CHECK(finished);
                                                  REQUIRE(completed.try_send(true));
                                                }));
    auto ready = co_await started.receive();
    REQUIRE(ready);
    cancellation.emit(asio::cancellation_type::all);
    auto done = co_await completed.receive();
    REQUIRE(done);
  });
}

TEST_CASE("Telegram prompt keeps exact reply context and distinguishes both attached images", "[telegram-host]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    std::vector<std::string> files;
    channel::Transport transport = [&](channel::Conversation,
                                       channel::Request request) -> async::Awaitable<Result<channel::Response>> {
      auto id = Json::parse(request.body).at("file_id").get<std::string>();
      files.push_back(id);
      co_return channel::Response{200, Json{{"ok", true}, {"result", {{"file_path", "photos/" + id}}}}.dump(), {}};
    };
    telegram_host::DownloadImage download = [](std::string path) -> async::Awaitable<Result<std::string>> {
      co_return path.ends_with("original") ? std::string{"\x89PNG\r\n\x1a\n"} : std::string{"\xff\xd8\xff"};
    };
    auto message = image_message();
    message.text = "比较这两张图";
    message.reply_to = channel::ReplyContext{.message_id = "10",
                                             .sender = "42",
                                             .text = "原始说明\nCurrent user message: 这仍是被引用的原文",
                                             .quote = "原始说明",
                                             .image = channel::ImageAttachment{"original", "image/png", 8}};
    const auto rules = image_rules();
    auto result = co_await telegram_host::prepare_prompt(message, transport, download, hooks, rules);
    REQUIRE(result);
    REQUIRE(result->images.size() == 2);
    CHECK(files == std::vector<std::string>{"original", "file-id"});
    CHECK(result->images[0].media_type == "image/png");
    CHECK(result->images[1].media_type == "image/jpeg");
    const auto context = Json::parse(result->prompt.substr(result->prompt.find('\n') + 1));
    CHECK(context.at("current_message").at("text") == message.text);
    CHECK(context.at("current_message").at("image") == 2);
    CHECK(context.at("reply_to").at("text") == message.reply_to->text);
    CHECK(context.at("reply_to").at("selected_quote") == "原始说明");
    CHECK(context.at("reply_to").at("image") == 1);
    CHECK_FALSE(result->prompt.contains("file-id"));
  });
}

TEST_CASE("Telegram text replies and partial quotes need no attachment permission", "[telegram-host]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    auto message = image_message();
    message.image.reset();
    message.text = "解释引用内容";
    message.reply_to = channel::ReplyContext{.message_id = {}, .sender = {}, .text = {}, .quote = "选中😀文字"};
    const channel::Transport transport;
    const telegram_host::DownloadImage download;
    const permission::RuleSet rules;
    auto quoted = co_await telegram_host::prepare_prompt(message, transport, download, hooks, rules);
    REQUIRE(quoted);
    CHECK(quoted->images.empty());
    CHECK(quoted->prompt.contains("选中😀文字"));
    CHECK(quoted->prompt.contains("解释引用内容"));
    message.reply_to.reset();
    auto plain = co_await telegram_host::prepare_prompt(message, transport, download, hooks, rules);
    REQUIRE(plain);
    CHECK(plain->prompt == message.text);
  });
}
