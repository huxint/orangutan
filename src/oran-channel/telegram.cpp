#include "protocol.hpp"
#include <algorithm>
#include <charconv>

namespace orangutan::channel {
namespace {
using namespace detail;
Result<std::optional<ImageAttachment>> image_attachment(const Json& message) {
  if (message.contains("photo")) {
    const auto& photos = message.at("photo").get_ref<const Json::array_t&>();
    if (photos.empty())
      return std::unexpected(Error::parsing("empty Telegram photo"));
    const auto& photo = *std::ranges::max_element(photos, {}, [](const Json& size) {
      return size.at("width").get<std::uint64_t>() * size.at("height").get<std::uint64_t>();
    });
    return std::optional{ImageAttachment{photo.at("file_id").get<std::string>(),
                                         "image/jpeg",
                                         photo.value("file_size", std::uint64_t{})}};
  }
  if (message.contains("document")) {
    const auto& document = message.at("document");
    auto media = document.value("mime_type", std::string{});
    if (media.starts_with("image/"))
      return std::optional{ImageAttachment{document.at("file_id").get<std::string>(),
                                           std::move(media),
                                           document.value("file_size", std::uint64_t{})}};
  }
  return std::optional<ImageAttachment>{};
}

Result<std::optional<ReplyContext>> reply_context(const Json& message) {
  std::optional<ReplyContext> reply;
  if (message.contains("reply_to_message")) {
    const auto& original = message.at("reply_to_message");
    auto image = image_attachment(original);
    if (!image)
      return std::unexpected(image.error());
    reply = ReplyContext{.message_id = id(original.at("message_id")),
                         .sender = original.contains("from") ? id(original.at("from").at("id")) : "",
                         .text = original.value("text", original.value("caption", std::string{})),
                         .quote = {},
                         .image = std::move(*image)};
  } else if (message.contains("external_reply")) {
    const auto& original = message.at("external_reply");
    auto image = image_attachment(original);
    if (!image)
      return std::unexpected(image.error());
    reply = ReplyContext{.message_id = id(original.value("message_id", Json{})),
                         .sender = {},
                         .text = {},
                         .quote = {},
                         .image = std::move(*image)};
  }
  if (message.contains("quote")) {
    if (!reply)
      reply.emplace();
    reply->quote = message.at("quote").at("text").get<std::string>();
  }
  return reply;
}

Result<std::int64_t> number(std::string_view text) {
  std::int64_t value{};
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::unexpected(Error::invalid_argument("invalid Telegram numeric identifier"));
  return value;
}
class Telegram final : public Adapter {
public:
  Platform platform() const noexcept override {
    return Platform::telegram;
  }
  Capabilities capabilities(ChatKind) const noexcept override {
    return {Activity::renewable, true, 4000};
  }
  Result<std::optional<Message>> decode(std::string_view envelope, const Account& account) const override {
    auto parsed = parse(envelope);
    if (!parsed)
      return std::unexpected(parsed.error());
    try {
      if (!parsed->contains("message"))
        return std::optional<Message>{};
      const auto& msg = parsed->at("message");
      auto image = image_attachment(msg);
      if (!image)
        return std::unexpected(image.error());
      if (!msg.contains("text") && !*image)
        return std::optional<Message>{};
      const auto& from = msg.at("from");
      if (from.value("is_bot", false))
        return std::optional<Message>{};
      const auto& chat = msg.at("chat");
      auto kind = chat.at("type").get<std::string>();
      if (kind != "private" && kind != "group" && kind != "supergroup")
        return std::optional<Message>{};
      auto reply = reply_context(msg);
      if (!reply)
        return std::unexpected(reply.error());
      Message m{{platform(),
                 account.id,
                 kind == "private" ? ChatKind::direct : ChatKind::group,
                 id(chat.at("id")),
                 id(msg.value("message_thread_id", Json{}))},
                id(parsed->at("update_id")),
                id(msg.at("message_id")),
                id(from.at("id")),
                msg.value("text", msg.value("caption", std::string{})),
                std::move(*image),
                std::move(*reply)};
      return checked(std::move(m));
    } catch (const Json::exception&) {
      return std::unexpected(Error::parsing("invalid Telegram message"));
    }
  }
  Result<Request> reply(const Message& m, std::string_view text, std::size_t) const override {
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    auto chat = number(m.conversation.chat);
    auto parent = number(m.message_id);
    if (!chat || !parent)
      return std::unexpected(Error::invalid_argument("invalid Telegram reply route"));
    Json body{{"chat_id", *chat},
              {"text", text},
              {"reply_parameters", {{"message_id", *parent}}},
              {"link_preview_options", {{"is_disabled", true}}}};
    if (!m.conversation.thread.empty()) {
      auto thread = number(m.conversation.thread);
      if (!thread)
        return std::unexpected(thread.error());
      body["message_thread_id"] = *thread;
    }
    return Request{"POST", "/sendMessage", body.dump()};
  }
  Result<std::optional<Request>> activity(const Message& m, std::optional<Receipt> receipt) const override {
    if (receipt)
      return std::optional<Request>{};  // Telegram has no stop action; it expires.
    auto valid = validate(m, platform());
    if (!valid)
      return std::unexpected(valid.error());
    auto chat = number(m.conversation.chat);
    if (!chat)
      return std::unexpected(chat.error());
    Json body{{"chat_id", *chat}, {"action", "typing"}};
    if (!m.conversation.thread.empty()) {
      auto thread = number(m.conversation.thread);
      if (!thread)
        return std::unexpected(thread.error());
      body["message_thread_id"] = *thread;
    }
    return std::optional{Request{"POST", "/sendChatAction", body.dump()}};
  }
  Result<Receipt> accept(const Response& response) const override {
    return detail::accept(platform(), response);
  }
};
}  // namespace
const Adapter& telegram() noexcept {
  static const Telegram adapter;
  return adapter;
}
}  // namespace orangutan::channel
