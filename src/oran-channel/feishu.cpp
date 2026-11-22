#include "protocol.hpp"

namespace orangutan::channel {
namespace {
using namespace detail;
class Feishu final : public Adapter {
public:
  Platform platform() const noexcept override {
    return Platform::feishu;
  }
  Capabilities capabilities(ChatKind) const noexcept override {
    return {Activity::reaction, true, 4000};
  }
  Result<std::optional<Message>> decode(std::string_view envelope, const Account& account) const override {
    auto parsed = parse(envelope);
    if (!parsed)
      return std::unexpected(parsed.error());
    try {
      if (!parsed->contains("header"))
        return std::optional<Message>{};
      const auto& header = parsed->at("header");
      if (header.at("event_type") != "im.message.receive_v1")
        return std::optional<Message>{};
      const auto& event = parsed->at("event");
      const auto& sender = event.at("sender");
      if (sender.at("sender_type") != "user")
        return std::optional<Message>{};
      const auto& msg = event.at("message");
      if (msg.at("message_type") != "text")
        return std::optional<Message>{};
      auto content = parse(msg.at("content").get<std::string>());
      if (!content)
        return std::unexpected(content.error());
      auto text = content->at("text").get<std::string>();
      if (!account.bot_id.empty() && msg.contains("mentions")) {
        for (const auto& mention : msg.at("mentions")) {
          if (mention.at("id").value("open_id", std::string{}) != account.bot_id)
            continue;
          auto key = mention.at("key").get<std::string>();
          if (key.empty())
            continue;
          for (auto pos = text.find(key); pos != std::string::npos; pos = text.find(key))
            text.erase(pos, key.size());
        }
      }
      if (auto pos = text.find_first_not_of(" \t\r\n"); pos != std::string::npos)
        text.erase(0, pos);
      else
        return std::optional<Message>{};
      auto kind = msg.at("chat_type").get<std::string>();
      if (kind != "p2p" && kind != "group")
        return std::optional<Message>{};
      Message m{{platform(),
                 account.id,
                 kind == "p2p" ? ChatKind::direct : ChatKind::group,
                 id(msg.at("chat_id")),
                 id(msg.value("thread_id", msg.value("root_id", Json{})))},
                id(header.at("event_id")),
                id(msg.at("message_id")),
                id(sender.at("sender_id").at("open_id")),
                std::move(text)};
      return checked(std::move(m));
    } catch (const Json::exception&) {
      return std::unexpected(Error::parsing("invalid Feishu message"));
    }
  }
  Result<Request> reply(const Message& m, std::string_view text, std::size_t) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    Json body{{"msg_type", "text"}, {"content", Json{{"text", text}}.dump()}};
    if (!m.conversation.thread.empty())
      body["reply_in_thread"] = true;
    return Request{"POST", "/open-apis/im/v1/messages/" + segment(m.message_id) + "/reply", body.dump()};
  }
  Result<std::optional<Request>> activity(const Message& m, std::optional<Receipt> receipt) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    auto path = "/open-apis/im/v1/messages/" + segment(m.message_id) + "/reactions";
    if (receipt) {
      if (receipt->id.empty())
        return std::optional<Request>{};
      return std::optional{Request{"DELETE", path + "/" + segment(receipt->id), {}}};
    }
    return std::optional{Request{"POST", path, Json{{"reaction_type", {{"emoji_type", "Typing"}}}}.dump()}};
  }
  Result<Receipt> accept(const Response& response) const override {
    return detail::accept(platform(), response);
  }
};
}  // namespace
const Adapter& feishu() noexcept {
  static const Feishu adapter;
  return adapter;
}
}  // namespace orangutan::channel
