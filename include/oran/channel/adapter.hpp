#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::channel {

enum class Platform : std::uint8_t {
  telegram,
  qq,
  feishu
};
enum class ChatKind : std::uint8_t {
  direct,
  group
};
enum class Activity : std::uint8_t {
  none,
  renewable,
  reaction,
  expiring
};

struct Capabilities {
  Activity activity{Activity::none};
  bool threads{false};
  std::size_t text_bytes{2000};
};

struct Conversation {
  Platform platform{Platform::telegram};
  std::string account;
  ChatKind kind{ChatKind::direct};
  std::string chat;
  std::string thread;
  friend bool operator==(const Conversation&, const Conversation&) = default;
};

struct ImageAttachment {
  std::string file_id;
  std::string media_type;
  std::uint64_t file_size{0};
};

struct ReplyContext {
  std::string message_id;
  std::string sender;
  std::string text;
  std::string quote;
  std::optional<ImageAttachment> image{};
  std::string reference_key{};
};

struct Message {
  Conversation conversation;
  std::string event_id;
  std::string message_id;
  std::string sender;
  std::string text;
  std::optional<ImageAttachment> image{};
  std::optional<ReplyContext> reply_to{};
  /// Provider reference index, distinct from its delivery/message ID.
  std::string reference_key{};
};

/// Account and bot identity come from trusted host configuration, not the event.
struct Account {
  std::string id;
  std::string bot_id;
};

/// Credential-free relative API request. Only the transport attaches secrets.
struct Request {
  std::string method{"POST"};
  std::string path;
  std::string body;
};

struct Response {
  std::uint16_t status{200};
  std::string body;
  std::optional<std::chrono::milliseconds> retry_after;
};

struct Receipt {
  std::string id;
  std::string reference_key{};
};

/// Stateless protocol strategy. Ingress must already be authenticated by the host.
/// Unsupported events return an empty optional; malformed supported events fail.
class Adapter {
public:
  virtual ~Adapter() = default;
  [[nodiscard]] virtual Platform platform() const noexcept = 0;
  [[nodiscard]] virtual Capabilities capabilities(ChatKind kind = ChatKind::direct) const noexcept = 0;
  [[nodiscard]] virtual core::Result<std::optional<Message>> decode(std::string_view envelope,
                                                                    const Account& account) const = 0;
  [[nodiscard]] virtual core::Result<Request>
  reply(const Message& message, std::string_view text, std::size_t part) const = 0;
  /// Empty receipt starts activity; a receipt removes the exact created reaction.
  [[nodiscard]] virtual core::Result<std::optional<Request>> activity(const Message& message,
                                                                      std::optional<Receipt> receipt) const = 0;
  [[nodiscard]] virtual core::Result<Receipt> accept(const Response& response) const = 0;
};

[[nodiscard]] const Adapter& telegram() noexcept;
[[nodiscard]] const Adapter& qq() noexcept;
[[nodiscard]] const Adapter& feishu() noexcept;
/// QQ native Markdown, retaining passive-reply sequence and size checks.
[[nodiscard]] core::Result<Request> qq_markdown_reply(const Message& message, std::string_view text, std::size_t part);
[[nodiscard]] std::string conversation_key(const Conversation& conversation);
/// Validate UTF-8 and split conservatively by bytes, never inside a code point.
[[nodiscard]] core::Result<std::vector<std::string>> split_text(std::string_view text, std::size_t max_bytes);

}  // namespace orangutan::channel
