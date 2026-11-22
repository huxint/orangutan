#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <oran/channel/adapter.hpp>
#include <oran/core/error.hpp>

namespace channel = orangutan::channel;
using Json = nlohmann::json;

TEST_CASE("Telegram preserves account chat topic and reply identity") {
  auto result = channel::telegram().decode(
      R"({"update_id":19,"message":{"message_id":23,"message_thread_id":42,"chat":{"id":-100123,"type":"supergroup"},"from":{"id":9,"is_bot":false},"text":"hello"}})",
      {"bot-one", "7"});
  REQUIRE(result);
  REQUIRE(*result);
  const auto& m = **result;
  CHECK(m.conversation.chat == "-100123");
  CHECK(m.conversation.thread == "42");
  CHECK(m.conversation.account == "bot-one");
  auto reply = channel::telegram().reply(m, "<plain> *text*", 0);
  REQUIRE(reply);
  auto body = Json::parse(reply->body);
  CHECK(body["chat_id"] == -100123);
  CHECK(body["message_thread_id"] == 42);
  CHECK(body["reply_parameters"]["message_id"] == 23);
  CHECK_FALSE(body.contains("parse_mode"));
  auto typing = channel::telegram().activity(m, {});
  REQUIRE(typing);
  REQUIRE(*typing);
  CHECK((**typing).path == "/sendChatAction");
  CHECK(Json::parse((**typing).body)["message_thread_id"] == 42);
  auto stop = channel::telegram().activity(m, channel::Receipt{});
  REQUIRE(stop);
  CHECK_FALSE(*stop);
  auto other = m.conversation;
  other.account = "bot-two";
  CHECK(channel::conversation_key(other) != channel::conversation_key(m.conversation));
  other = m.conversation;
  other.thread = "43";
  CHECK(channel::conversation_key(other) != channel::conversation_key(m.conversation));
}

TEST_CASE("QQ official group and C2C passive replies carry message sequences") {
  for (bool group : {false, true}) {
    Json event{{"op", 0},
               {"t", group ? "GROUP_AT_MESSAGE_CREATE" : "C2C_MESSAGE_CREATE"},
               {"d",
                {{"id", "incoming"},
                 {"content", " hello"},
                 {"group_openid", "group"},
                 {"author", {{group ? "member_openid" : "user_openid", "user"}}}}}};
    auto result = channel::qq().decode(event.dump(), {"account", "bot"});
    REQUIRE(result);
    REQUIRE(*result);
    auto reply = channel::qq().reply(**result, "answer", 1);
    REQUIRE(reply);
    CHECK(reply->path == (group ? "/v2/groups/group/messages" : "/v2/users/user/messages"));
    auto body = Json::parse(reply->body);
    CHECK(body["msg_id"] == "incoming");
    CHECK(body["msg_seq"] == (group ? 2 : 4));
    CHECK(body["msg_type"] == 0);
    CHECK_FALSE(channel::qq().reply(**result, "answer", 5));
    auto typing = channel::qq().activity(**result, {});
    REQUIRE(typing);
    if (group)
      CHECK_FALSE(*typing);
    else {
      REQUIRE(*typing);
      auto status = Json::parse((**typing).body);
      CHECK(status["msg_type"] == 6);
      CHECK(status["msg_seq"] == 1);
      CHECK(status["input_notify"]["input_type"] == 1);
      auto stop = channel::qq().activity(**result, channel::Receipt{"status"});
      REQUIRE(stop);
      REQUIRE(*stop);
      CHECK(Json::parse((**stop).body)["input_notify"]["input_type"] == 2);
    }
  }
}

TEST_CASE("Feishu decodes user text and removes only its own typing reaction") {
  auto result = channel::feishu().decode(
      R"({"schema":"2.0","header":{"event_id":"evt","event_type":"im.message.receive_v1"},"event":{"sender":{"sender_type":"user","sender_id":{"open_id":"user"}},"message":{"message_id":"om_1","chat_id":"oc_1","chat_type":"group","thread_id":"omt_1","message_type":"text","content":"{\"text\":\"@_user_1 hello\"}","mentions":[{"key":"@_user_1","id":{"open_id":"bot"}}]}}})",
      {"account", "bot"});
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((**result).text == "hello");
  auto reply = channel::feishu().reply(**result, "中文 \"quoted\"", 0);
  REQUIRE(reply);
  CHECK(reply->path == "/open-apis/im/v1/messages/om_1/reply");
  auto body = Json::parse(reply->body);
  CHECK(body["reply_in_thread"] == true);
  CHECK(Json::parse(body["content"].get<std::string>())["text"] == "中文 \"quoted\"");
  auto start = channel::feishu().activity(**result, {});
  REQUIRE(start);
  REQUIRE(*start);
  CHECK(Json::parse((**start).body)["reaction_type"]["emoji_type"] == "Typing");
  auto receipt = channel::feishu().accept({200, R"({"code":0,"data":{"reaction_id":"reaction/1"}})", {}});
  REQUIRE(receipt);
  auto stop = channel::feishu().activity(**result, *receipt);
  REQUIRE(stop);
  REQUIRE(*stop);
  CHECK((**stop).method == "DELETE");
  CHECK((**stop).path.ends_with("/reactions/reaction%2F1"));
}

TEST_CASE("Channel decoders reject malformed supported events and ignore unrelated events") {
  for (auto adapter : {&channel::telegram(), &channel::qq(), &channel::feishu()}) {
    CHECK_FALSE(adapter->decode("not json", {"account", {}}));
    auto ignored = adapter->decode("{}", {"account", {}});
    REQUIRE(ignored);
    CHECK_FALSE(*ignored);
  }
  CHECK_FALSE(channel::telegram().decode(R"({"message":{"text":4}})", {"account", {}}));
  CHECK_FALSE(channel::qq().decode(R"({"op":0,"t":"C2C_MESSAGE_CREATE","d":null})", {"account", {}}));
  CHECK_FALSE(
      channel::feishu().decode(R"({"header":{"event_type":"im.message.receive_v1"},"event":null})", {"account", {}}));
  auto bot = channel::telegram().decode(R"({"message":{"text":"loop","from":{"is_bot":true}}})", {"account", {}});
  REQUIRE(bot);
  CHECK_FALSE(*bot);
}

TEST_CASE("Channel API failures do not echo upstream bodies and preserve retry delays") {
  auto limited = channel::telegram().accept(
      {200, R"({"ok":false,"error_code":429,"description":"SECRET","parameters":{"retry_after":7}})", {}});
  REQUIRE_FALSE(limited);
  CHECK(limited.error().kind() == orangutan::core::ErrorKind::rate_limit);
  CHECK(limited.error().retry_after() == std::chrono::seconds{7});
  CHECK_FALSE(limited.error().message().contains("SECRET"));
  CHECK_FALSE(channel::feishu().accept({200, R"({"code":999,"msg":"SECRET"})", {}}));
  CHECK_FALSE(channel::qq().accept({200, R"({"code":123,"message":"SECRET"})", {}}));
  CHECK_FALSE(channel::telegram().accept({200, R"({"ok":"true"})", {}}));
  auto status = channel::qq().accept({200, "{}", {}});
  REQUIRE(status);
  CHECK(status->id.empty());
}

TEST_CASE("Telegram receives the largest photo and image documents with captions") {
  auto photo = channel::telegram().decode(R"({"update_id":7,"message":{"message_id":9,
    "from":{"id":42,"is_bot":false},"chat":{"id":42,"type":"private"},"caption":"看这张图",
    "photo":[{"file_id":"large","width":800,"height":600,"file_size":1234},
             {"file_id":"small","width":80,"height":60}]}})",
                                          {"bot", "1"});
  REQUIRE(photo);
  REQUIRE(*photo);
  REQUIRE((**photo).image);
  CHECK((**photo).image->file_id == "large");
  CHECK((**photo).image->file_size == 1234);
  CHECK((**photo).text == "看这张图");
  auto document = channel::telegram().decode(R"({"update_id":8,"message":{"message_id":10,
    "from":{"id":42},"chat":{"id":42,"type":"private"},
    "document":{"file_id":"png-file","mime_type":"image/png","file_size":500}}})",
                                             {"bot", "1"});
  REQUIRE(document);
  REQUIRE(*document);
  REQUIRE((**document).image);
  CHECK((**document).text.empty());
  CHECK((**document).image->media_type == "image/png");
  auto unsupported =
      channel::telegram().decode(R"({"message":{"document":{"mime_type":"application/pdf"}}})", {"bot", "1"});
  REQUIRE(unsupported);
  CHECK_FALSE(*unsupported);
}

TEST_CASE("Telegram retains reply text and exact selected quotes independently of bot authorship") {
  auto decoded = channel::telegram().decode(R"({"update_id":20,"message":{"message_id":30,
    "from":{"id":42},"chat":{"id":42,"type":"private"},"text":"解释这句",
    "reply_to_message":{"message_id":29,"from":{"id":1,"is_bot":true},
                        "text":"😀第一句。第二句。","reply_to_message":{"text":"ignore nested reply"}},
    "quote":{"text":"第二句。","position":7,"is_manual":true}}})",
                                            {"bot", "1"});
  REQUIRE(decoded);
  REQUIRE(*decoded);
  const auto& message = **decoded;
  REQUIRE(message.reply_to);
  CHECK(message.text == "解释这句");
  CHECK(message.reply_to->message_id == "29");
  CHECK(message.reply_to->sender == "1");
  CHECK(message.reply_to->text == "😀第一句。第二句。");
  CHECK(message.reply_to->quote == "第二句。");
}

TEST_CASE("Telegram retains replied images and quote-only external replies") {
  auto decoded = channel::telegram().decode(R"({"update_id":21,"message":{"message_id":31,
    "from":{"id":42},"chat":{"id":42,"type":"private"},"text":"看原图",
    "reply_to_message":{"message_id":29,"from":{"id":42},"caption":"原图说明",
      "photo":[{"file_id":"old-photo","width":800,"height":600}]}}})",
                                            {"bot", "1"});
  REQUIRE(decoded);
  REQUIRE(*decoded);
  REQUIRE((**decoded).reply_to);
  REQUIRE((**decoded).reply_to->image);
  CHECK((**decoded).reply_to->image->file_id == "old-photo");
  CHECK((**decoded).reply_to->text == "原图说明");
  CHECK_FALSE((**decoded).image);

  auto quoted = channel::telegram().decode(R"({"update_id":22,"message":{"message_id":32,
    "from":{"id":42},"chat":{"id":42,"type":"private"},"text":"这句呢",
    "quote":{"text":"精确选中的内容","position":15}}})",
                                           {"bot", "1"});
  REQUIRE(quoted);
  REQUIRE(*quoted);
  REQUIRE((**quoted).reply_to);
  CHECK((**quoted).reply_to->quote == "精确选中的内容");
  CHECK((**quoted).reply_to->text.empty());
}

TEST_CASE("Channel text splitting preserves Unicode and rejects malformed UTF-8") {
  const std::string text = "你好🙂world";
  auto parts = channel::split_text(text, 4);
  REQUIRE(parts);
  std::string joined;
  for (const auto& part : *parts) {
    CHECK(part.size() <= 4);
    joined += part;
  }
  CHECK(joined == text);
  CHECK_FALSE(channel::split_text(std::string{"\xc0\x80", 2}, 4));
  CHECK_FALSE(channel::split_text(std::string{"\xed\xa0\x80", 3}, 4));
  CHECK_FALSE(channel::split_text(std::string{"\xf4\x90\x80\x80", 4}, 4));
  CHECK_FALSE(channel::split_text("x", 0));
}

TEST_CASE("QQ image-only events and official reference indices survive decoding") {
  Json event{{"op", 0},
             {"t", "C2C_MESSAGE_CREATE"},
             {"d",
              {{"id", "message"},
               {"content", ""},
               {"author", {{"user_openid", "owner"}}},
               {"attachments",
                Json::array({{{"content_type", "image/jpeg"},
                              {"size", 42},
                              {"url", "//multimedia.nt.qq.com.cn/download?fileid=fixture"}}})},
               {"message_scene", {{"ext", {"msg_idx=own-index", "ref_msg_idx=previous-index"}}}}}}};
  auto result = channel::qq().decode(event.dump(), {"app", {}});
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((**result).text.empty());
  REQUIRE((**result).image);
  CHECK((**result).image->file_id == "https://multimedia.nt.qq.com.cn/download?fileid=fixture");
  CHECK((**result).image->file_size == 42);
  CHECK((**result).reference_key == "own-index");
  REQUIRE((**result).reply_to);
  CHECK((**result).reply_to->reference_key == "previous-index");
  event["d"]["message_type"] = 103;
  event["d"]["msg_elements"] = Json::array({{{"msg_idx", "quoted-index"}, {"content", "inline quoted text"}}});
  result = channel::qq().decode(event.dump(), {"app", {}});
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((**result).reply_to->reference_key == "quoted-index");
  CHECK((**result).reply_to->text == "inline quoted text");
  event["d"]["author"]["bot"] = true;
  result = channel::qq().decode(event.dump(), {"app", {}});
  REQUIRE(result);
  CHECK_FALSE(*result);
}

TEST_CASE("QQ native Markdown retains passive sequencing and confirmed reference indices") {
  Json event{{"op", 0},
             {"t", "C2C_MESSAGE_CREATE"},
             {"d", {{"id", "message"}, {"content", "hello"}, {"author", {{"user_openid", "owner"}}}}}};
  auto decoded = channel::qq().decode(event.dump(), {"app", {}});
  REQUIRE(decoded);
  REQUIRE(*decoded);
  auto request = channel::qq_markdown_reply(**decoded, "**bold**", 0);
  REQUIRE(request);
  auto body = Json::parse(request->body);
  CHECK(body.at("msg_type") == 2);
  CHECK(body.at("markdown").at("content") == "**bold**");
  CHECK(body.at("msg_seq") == 3);
  CHECK(body.at("msg_id") == "message");
  CHECK_FALSE(body.contains("content"));
  CHECK_FALSE(channel::qq_markdown_reply(**decoded, "text", 5));
  auto receipt = channel::qq().accept({200, R"({"id":"delivered","ext_info":{"ref_idx":"reference"}})", {}});
  REQUIRE(receipt);
  CHECK(receipt->id == "delivered");
  CHECK(receipt->reference_key == "reference");
}
