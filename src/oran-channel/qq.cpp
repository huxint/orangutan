#include "protocol.hpp"

namespace orangutan::channel {
namespace {
using namespace detail;
class QQ final : public Adapter {
public:
  Platform platform() const noexcept override {
    return Platform::qq;
  }
  Capabilities capabilities(ChatKind kind) const noexcept override {
    return {kind == ChatKind::direct ? Activity::expiring : Activity::none, false, 2000};
  }
  Result<std::optional<Message>> decode(std::string_view envelope, const Account& account) const override {
    auto parsed = parse(envelope);
    if (!parsed)
      return std::unexpected(parsed.error());
    try {
      if (parsed->value("op", -1) != 0)
        return std::optional<Message>{};
      auto type = parsed->at("t").get<std::string>();
      if (type != "C2C_MESSAGE_CREATE" && type != "GROUP_AT_MESSAGE_CREATE")
        return std::optional<Message>{};
      const auto& msg = parsed->at("d");
      const auto& author = msg.at("author");
      bool group = type == "GROUP_AT_MESSAGE_CREATE";
      auto sender = id(author.at(group ? "member_openid" : "user_openid"));
      if (author.value("bot", false))
        return std::optional<Message>{};
      auto text = msg.value("content", std::string{});
      std::optional<ImageAttachment> image;
      if (msg.contains("attachments")) {
        for (const auto& attachment : msg.at("attachments")) {
          const auto media = attachment.value("content_type", std::string{});
          if (!image && media.starts_with("image/")) {
            auto url = attachment.at("url").get<std::string>();
            if (url.starts_with("//"))
              url.insert(0, "https:");
            image = ImageAttachment{std::move(url), media, attachment.value("size", std::uint64_t{})};
          }
        }
      }
      if (!account.bot_id.empty()) {
        const auto mention = "<@!" + account.bot_id + ">";
        if (text.starts_with(mention))
          text.erase(0, mention.size());
      }
      if (auto pos = text.find_first_not_of(" \t\r\n"); pos != std::string::npos)
        text.erase(0, pos);
      else
        text.clear();
      if (text.empty() && !image)
        return std::optional<Message>{};
      auto message_id = id(msg.at("id"));
      Message m{{platform(),
                 account.id,
                 group ? ChatKind::group : ChatKind::direct,
                 group ? id(msg.at("group_openid")) : sender,
                 {}},
                message_id,
                message_id,
                sender,
                std::move(text)};
      m.image = std::move(image);
      std::string reference, quoted_text;
      if (msg.contains("message_scene") && msg.at("message_scene").contains("ext")) {
        for (const auto& entry : msg.at("message_scene").at("ext")) {
          if (!entry.is_string())
            continue;
          const auto& value = entry.get_ref<const std::string&>();
          if (value.starts_with("msg_idx="))
            m.reference_key = value.substr(8);
          if (value.starts_with("ref_msg_idx="))
            reference = value.substr(12);
        }
      }
      if (msg.value("message_type", 0) == 103 && msg.contains("msg_elements")) {
        for (const auto& element : msg.at("msg_elements")) {
          if (element.is_object() && element.contains("msg_idx")) {
            reference = element.at("msg_idx").get<std::string>();
            quoted_text = element.value("content", std::string{});
            break;
          }
        }
      }
      if (!reference.empty()) {
        m.reply_to.emplace();
        m.reply_to->reference_key = std::move(reference);
        m.reply_to->text = std::move(quoted_text);
      } else if (msg.contains("message_reference")) {
        m.reply_to.emplace();
        m.reply_to->message_id = id(msg.at("message_reference").at("message_id"));
      }
      return checked(std::move(m));
    } catch (const Json::exception&) {
      return std::unexpected(Error::parsing("invalid QQ message"));
    }
  }
  Result<Request> reply(const Message& m, std::string_view text, std::size_t part) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    if (!m.conversation.thread.empty() || part >= 5)
      return std::unexpected(Error::invalid_argument("QQ passive replies support at most five parts and no threads"));
    return Request{"POST",
                   std::string{m.conversation.kind == ChatKind::group ? "/v2/groups/" : "/v2/users/"} +
                       segment(m.conversation.chat) + "/messages",
                   Json{{"content", text},
                        {"msg_type", 0},
                        {"msg_id", m.message_id},
                        {"msg_seq", part + (m.conversation.kind == ChatKind::direct ? 3 : 1)}}
                       .dump()};
  }
  Result<std::optional<Request>> activity(const Message& m, std::optional<Receipt> receipt) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    if (m.conversation.kind != ChatKind::direct)
      return std::optional<Request>{};
    return std::optional{Request{"POST",
                                 "/v2/users/" + segment(m.conversation.chat) + "/messages",
                                 Json{{"msg_type", 6},
                                      {"msg_id", m.message_id},
                                      {"msg_seq", receipt ? 2 : 1},
                                      {"input_notify", {{"input_type", receipt ? 2 : 1}, {"input_second", 60}}}}
                                     .dump()}};
  }
  Result<Receipt> accept(const Response& response) const override {
    return detail::accept(platform(), response);
  }
};
}  // namespace
const Adapter& qq() noexcept {
  static const QQ adapter;
  return adapter;
}
core::Result<Request> qq_markdown_reply(const Message& message, std::string_view text, std::size_t part) {
  auto request = qq().reply(message, text, part);
  if (!request)
    return std::unexpected(request.error());
  auto body = nlohmann::json::parse(request->body);
  body.erase("content");
  body["msg_type"] = 2;
  body["markdown"] = {{"content", text}};
  request->body = body.dump();
  return request;
}
}  // namespace orangutan::channel
