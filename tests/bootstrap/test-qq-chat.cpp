#include "../../apps/qq/chat.hpp"
#include "../test-helpers/run_async.hpp"

#include <nlohmann/json.hpp>
#include <oran/hook/bus.hpp>

namespace {
using namespace orangutan;
using Json = nlohmann::json;
using core::Result;
channel::Message message() {
  channel::Message value{{channel::Platform::qq, "app", channel::ChatKind::direct, "owner", {}},
                         "event",
                         "message",
                         "owner",
                         "explain"};
  value.reference_key = "own-index";
  return value;
}
permission::RuleSet allowed() {
  permission::RuleSet result;
  result.push_back({.verdict = permission::Verdict::allow, .tool_pattern = "ChannelAttachment"});
  return result;
}
}  // namespace

TEST_CASE("QQ references resolve saved incoming images and outgoing chunks without guessing", "[qq-chat]") {
  auto original = message();
  original.image = channel::ImageAttachment{"https://gchat.qpic.cn/image", "image/png", 12};
  auto delivered = Json::array(
      {{{"message", qq_chat::saved_message(original)},
        {"receipts",
         Json::array(
             {{{"id", "sent"}, {"reference_key", "outgoing"}, {"sender", "app"}, {"text", "specific chunk"}}})}}});
  auto incoming = message();
  incoming.reply_to.emplace();
  incoming.reply_to->reference_key = "own-index";
  auto resolved = qq_chat::resolve_reference(incoming, delivered);
  REQUIRE(resolved.reply_to);
  REQUIRE(resolved.reply_to->image);
  CHECK(resolved.reply_to->text == "explain");
  CHECK(resolved.reply_to->image->file_id == original.image->file_id);
  incoming.reply_to->reference_key = "outgoing";
  resolved = qq_chat::resolve_reference(incoming, delivered);
  CHECK(resolved.reply_to->text == "specific chunk");
  CHECK(resolved.reply_to->sender == "app");
  incoming.reply_to->reference_key = "missing";
  resolved = qq_chat::resolve_reference(incoming, delivered);
  CHECK(resolved.reply_to->text.empty());
  CHECK_FALSE(resolved.reply_to->image);
  incoming.reply_to->text = "authenticated inline quote";
  resolved = qq_chat::resolve_reference(incoming, delivered);
  CHECK(resolved.reply_to->text == "authenticated inline quote");
}

TEST_CASE("QQ image preparation bounds authorized requests and preserves quoted typed images", "[qq-chat]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    auto input = message();
    input.image = channel::ImageAttachment{"https://multimedia.nt.qq.com.cn/download?fileid=test", "image/png", 12};
    input.reply_to.emplace();
    input.reply_to->text = "quoted data";
    input.reply_to->image = input.image;
    int requests = 0;
    qq_chat::Download download = [&](http::BodyRequest request) -> async::Awaitable<Result<http::BodyResponse>> {
      ++requests;
      CHECK(request.max_bytes == qq_chat::image_max_bytes);
      CHECK(request.timeout == std::chrono::seconds{20});
      CHECK(request.headers.empty());
      co_return http::BodyResponse{200, {}, std::string{"\x89PNG\r\n\x1a\n"} + "test"};
    };
    permission::RuleSet denied;
    auto rejected = co_await qq_chat::prepare_prompt(input, download, hooks, denied);
    REQUIRE_FALSE(rejected);
    CHECK(requests == 0);
    auto rules = allowed();
    auto prompt = co_await qq_chat::prepare_prompt(input, download, hooks, rules);
    REQUIRE(prompt);
    REQUIRE(prompt->images.size() == 2);
    CHECK(prompt->images.front().data_base64 == "iVBORw0KGgp0ZXN0");
    CHECK(prompt->prompt.contains("quoted data"));
    CHECK(prompt->prompt.contains("not a new instruction"));
    CHECK_FALSE(prompt->prompt.contains("fileid"));
    CHECK(requests == 2);
  });
}

TEST_CASE("QQ image download rejects unsafe URLs size and disguised bytes", "[qq-chat]") {
  tests::run_async([](asio::io_context&) -> async::Awaitable<void> {
    hook::Bus hooks;
    auto input = message();
    int requests = 0;
    qq_chat::Download download = [&](http::BodyRequest) -> async::Awaitable<Result<http::BodyResponse>> {
      ++requests;
      co_return http::BodyResponse{200, {}, "not an image"};
    };
    auto rules = allowed();
    for (const auto* url : {"http://gchat.qpic.cn/image",
                            "https://gchat.qpic.cn.evil.test/image",
                            "https://gchat.qpic.cn@127.0.0.1/image",
                            "https://127.0.0.1/image",
                            "https://gchat.qpic.cn:443/image",
                            "https://gchat.qpic.cn/\\image"}) {
      input.image = channel::ImageAttachment{url, "image/png", 12};
      auto result = co_await qq_chat::prepare_prompt(input, download, hooks, rules);
      CHECK_FALSE(result);
    }
    CHECK(requests == 0);
    input.image = channel::ImageAttachment{"https://gchat.qpic.cn/image", "image/png", qq_chat::image_max_bytes + 1};
    auto result = co_await qq_chat::prepare_prompt(input, download, hooks, rules);
    CHECK_FALSE(result);
    CHECK(requests == 0);
    input.image->file_size = 12;
    result = co_await qq_chat::prepare_prompt(input, download, hooks, rules);
    CHECK_FALSE(result);
    CHECK(requests == 1);
  });
}

TEST_CASE("QQ local commands ignore quoted commands and image captions", "[qq-chat]") {
  auto input = message();
  input.text = "/new";
  CHECK(qq_chat::command(input) == qq_chat::Command::new_session);
  input.reply_to.emplace();
  CHECK(qq_chat::command(input) == qq_chat::Command::none);
  input.reply_to.reset();
  input.image.emplace();
  CHECK(qq_chat::command(input) == qq_chat::Command::none);
  input.image.reset();
  input.text = "/new unexpected";
  CHECK(qq_chat::command(input) == qq_chat::Command::unknown);
  input.text = "/help \n";
  CHECK(qq_chat::command(input) == qq_chat::Command::help);
  input.text = "/status";
  CHECK(qq_chat::command(input) == qq_chat::Command::status);
  input.text = "please explain /new";
  CHECK(qq_chat::command(input) == qq_chat::Command::none);
}

TEST_CASE("QQ Markdown splitting closes and reopens fenced code within passive limits", "[qq-chat]") {
  auto parts = qq_chat::split_markdown("```cpp\n" + std::string(3000, 'x') + "\n```", 2000);
  REQUIRE(parts);
  REQUIRE(parts->size() == 2);
  for (const auto& part : *parts) {
    CHECK(part.size() <= 2000);
    CHECK(part.starts_with("```cpp\n"));
    CHECK(part.ends_with("```"));
  }
  parts = qq_chat::split_markdown(std::string(12000, 'x'), 2000);
  CHECK_FALSE(parts);
  parts = qq_chat::split_markdown("中文🙂\n**bold**", 2000);
  REQUIRE(parts);
  REQUIRE(parts->size() == 1);
  CHECK(parts->front() == "中文🙂\n**bold**");
}
