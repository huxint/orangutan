#include "protocol.hpp"
#include <algorithm>
#include <format>
#include <oran/core/str.hpp>

namespace orangutan::channel {
std::string conversation_key(const Conversation& c) {
  return std::format("{}:{}:{}:{}:{}:{}:{}:{}",
                     static_cast<int>(c.platform),
                     c.account.size(),
                     c.account,
                     static_cast<int>(c.kind),
                     c.chat.size(),
                     c.chat,
                     c.thread.size(),
                     c.thread);
}

core::Result<std::vector<std::string>> split_text(std::string_view text, std::size_t limit) {
  if (limit < 4)
    return std::unexpected(core::Error::invalid_argument("text limit must be at least four bytes"));
  if (!core::str::is_valid_utf8(text))
    return std::unexpected(core::Error::invalid_argument("invalid UTF-8 text"));
  std::vector<std::string> parts;
  while (!text.empty()) {
    const auto part = core::str::truncate_to_code_point(text, limit);
    parts.emplace_back(part);
    text.remove_prefix(part.size());
  }
  return parts;
}

namespace detail {
Result<Json> parse(std::string_view text) {
  if (text.size() > 1024 * 1024)
    return std::unexpected(Error::parsing("channel envelope exceeds byte limit"));
  auto value = Json::parse(text, nullptr, false);
  if (value.is_discarded() || !value.is_object())
    return std::unexpected(Error::parsing("invalid channel JSON object"));
  return value;
}
std::string id(const Json& value) {
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_number_integer())
    return value.dump();
  return {};
}
Result<void> validate(const Message& m, Platform p) {
  if (m.conversation.platform != p || m.conversation.account.empty() || m.conversation.chat.empty() ||
      m.event_id.empty() || m.message_id.empty() || m.sender.empty() || (m.text.empty() && !m.image) ||
      m.conversation.account.size() > 256 || m.conversation.chat.size() > 256 || m.conversation.thread.size() > 256 ||
      m.reference_key.size() > 256 || m.event_id.size() > 256 || m.message_id.size() > 256 || m.sender.size() > 256 ||
      m.text.size() > 65536)
    return std::unexpected(Error::invalid_argument("invalid channel message identity or size"));
  if (!core::str::is_valid_utf8(m.text))
    return std::unexpected(Error::invalid_argument("invalid UTF-8 text"));
  const auto valid_image = [](const std::optional<ImageAttachment>& image) {
    return !image || (!image->file_id.empty() && image->file_id.size() <= 512);
  };
  if (!valid_image(m.image))
    return std::unexpected(Error::invalid_argument("invalid channel image identifier"));
  if (m.reply_to) {
    const auto& reply = *m.reply_to;
    if (reply.reference_key.size() > 256 || reply.message_id.size() > 256 || reply.sender.size() > 256 ||
        reply.text.size() > 65536 || reply.quote.size() > 65536 || !core::str::is_valid_utf8(reply.text) ||
        !core::str::is_valid_utf8(reply.quote) || !valid_image(reply.image))
      return std::unexpected(Error::invalid_argument("invalid channel reply context"));
  }
  return {};
}
Result<std::optional<Message>> checked(Message m) {
  auto valid = validate(m, m.conversation.platform);
  if (!valid)
    return std::unexpected(valid.error());
  return std::optional{std::move(m)};
}
std::string segment(std::string_view text) {
  std::string out;
  constexpr std::string_view hex = "0123456789ABCDEF";
  for (unsigned char ch : text) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')
      out += static_cast<char>(ch);
    else {
      out += '%';
      out += hex[ch >> 4];
      out += hex[ch & 15];
    }
  }
  return out;
}
Result<Receipt> accept(Platform p, const Response& response) {
  auto body = parse(response.body);
  bool limited = response.status == 429;
  if (body && p == Platform::telegram && body->contains("error_code"))
    limited |= (*body)["error_code"] == 429;
  if (limited) {
    auto error = Error::rate_limit("channel API rate limit");
    if (response.retry_after)
      error.with_retry_after(*response.retry_after);
    if (body && p == Platform::telegram && body->contains("parameters")) {
      const auto& params = (*body)["parameters"];
      if (params.is_object() && params.contains("retry_after") && params["retry_after"].is_number_unsigned()) {
        auto seconds = params["retry_after"].get<std::uint64_t>();
        error.with_retry_after(std::chrono::milliseconds{std::min(seconds, std::uint64_t{86400}) * 1000});
      }
    }
    return std::unexpected(std::move(error));
  }
  if (response.status == 401 || response.status == 403)
    return std::unexpected(Error{core::ErrorKind::auth, "channel API authorization failed"});
  if (response.status < 200 || response.status >= 300)
    return std::unexpected(
        Error::upstream("channel HTTP request failed").with("status", std::to_string(response.status)));
  if (!body)
    return std::unexpected(body.error());
  try {
    if (p == Platform::telegram) {
      if (!body->at("ok").get<bool>())
        return std::unexpected(Error::upstream("Telegram API rejected request"));
      const auto& result = body->at("result");
      if (!result.is_object() && result != true)
        return std::unexpected(Error::parsing("invalid Telegram success result"));
      return Receipt{result.is_object() ? id(result.value("message_id", Json{})) : std::string{}};
    }
    if (p == Platform::feishu) {
      if (body->at("code") != 0)
        return std::unexpected(Error::upstream("Feishu API rejected request"));
      auto data = body->value("data", Json::object());
      return Receipt{id(data.value("reaction_id", data.value("message_id", Json{})))};
    }
    if (body->contains("code") && body->at("code") != 0)
      return std::unexpected(Error::upstream("QQ API rejected request"));
    // QQ acknowledges input-status updates with an empty object. The
    // dispatcher separately requires a nonempty receipt for message delivery.
    if (body->empty())
      return Receipt{};
    auto message_id = id(body->at("id"));
    if (message_id.empty())
      return std::unexpected(Error::parsing("missing QQ message receipt"));
    auto reference = body->contains("ext_info") ? id(body->at("ext_info").value("ref_idx", Json{})) : std::string{};
    if (reference.size() > 256)
      return std::unexpected(Error::parsing("invalid QQ reference index"));
    return Receipt{std::move(message_id), std::move(reference)};
  } catch (const Json::exception&) {
    return std::unexpected(Error::parsing("invalid channel API response"));
  }
}
}  // namespace detail
}  // namespace orangutan::channel
