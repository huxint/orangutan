#include <algorithm>
#include <asio/awaitable.hpp>
#include <asio/this_coro.hpp>
#include <cctype>
#include <charconv>
#include <oran/bootstrap/channel_http.hpp>
#include <oran/core/error.hpp>
#include <oran/http/client.hpp>

namespace orangutan::bootstrap {
namespace {
bool path_segment(std::string_view value) {
  if (value.empty() || value.contains('/'))
    return false;
  std::string decoded;
  for (std::size_t i = 0; i < value.size(); ++i) {
    unsigned char byte = value[i];
    if (byte == '%') {
      if (value.size() - i < 3)
        return false;
      unsigned int number{};
      const auto* first = value.data() + i + 1;
      auto [end, error] = std::from_chars(first, first + 2, number, 16);
      if (error != std::errc{} || end != first + 2)
        return false;
      byte = static_cast<unsigned char>(number);
      i += 2;
    }
    if (byte == '/' || byte == '\\' || byte == 0)
      return false;
    decoded += static_cast<char>(byte);
  }
  return decoded != "." && decoded != "..";
}

bool message_operation(std::string_view path, std::string_view prefix, std::string_view operation) {
  if (!path.starts_with(prefix))
    return false;
  path.remove_prefix(prefix.size());
  const auto slash = path.find('/');
  return slash != std::string_view::npos && path_segment(path.substr(0, slash)) && path.substr(slash) == operation;
}

async::Awaitable<core::Result<channel::Response>> send_channel(http::Client& client,
                                                               const ChannelCredential& credential,
                                                               channel::Conversation conversation,
                                                               channel::Request request) {
  auto cancelled = co_await asio::this_coro::cancellation_state;
  if (cancelled.cancelled() != asio::cancellation_type::none)
    co_return std::unexpected(core::Error::cancelled());
  if (!credential || request.path.empty() || request.path.front() != '/' || request.path.starts_with("//") ||
      request.path.contains("..") || request.path.contains('?') || request.path.contains('#') ||
      request.path.contains('\\') ||
      std::ranges::any_of(request.path, [](unsigned char c) { return c <= 32 || c >= 127; }) ||
      (request.method != "POST" && request.method != "DELETE"))
    co_return std::unexpected(core::Error::invalid_argument("invalid channel HTTP binding request"));
  // Restrict this port to the protocol paths it implements, never arbitrary APIs.
  bool path_allowed = false;
  switch (conversation.platform) {
    case channel::Platform::telegram:
      path_allowed = request.method == "POST" && (request.path == "/sendMessage" || request.path == "/sendChatAction" ||
                                                  request.path == "/setMessageReaction" ||
                                                  request.path == "/sendMessageDraft" || request.path == "/getFile");
      break;
    case channel::Platform::qq:
      path_allowed = !conversation.account.empty() && conversation.account.size() <= 64 &&
                     std::ranges::all_of(conversation.account,
                                         [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }) &&
                     request.method == "POST" &&
                     (message_operation(request.path, "/v2/groups/", "/messages") ||
                      message_operation(request.path, "/v2/users/", "/messages"));
      break;
    case channel::Platform::feishu:
      if (request.method == "POST") {
        path_allowed = message_operation(request.path, "/open-apis/im/v1/messages/", "/reply") ||
                       message_operation(request.path, "/open-apis/im/v1/messages/", "/reactions");
      } else {
        const auto last = request.path.rfind('/');
        path_allowed = last != std::string::npos && path_segment(std::string_view{request.path}.substr(last + 1)) &&
                       message_operation(std::string_view{request.path}.substr(0, last),
                                         "/open-apis/im/v1/messages/",
                                         "/reactions");
      }
      break;
  }
  if (!path_allowed)
    co_return std::unexpected(core::Error::invalid_argument("unsupported channel HTTP path"));
  try {
    auto token = co_await credential(conversation);
    if (!token && token.error().kind() == core::ErrorKind::cancelled)
      co_return std::unexpected(core::Error::cancelled());
    if (!token || token->empty() || !std::ranges::all_of(*token, [platform = conversation.platform](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ':' || c == '_' ||
                 c == '-' || c == '.' ||
                 (platform != channel::Platform::telegram && (c == '~' || c == '+' || c == '/' || c == '='));
        }))
      co_return std::unexpected(core::Error{core::ErrorKind::auth, "channel credential unavailable or invalid"});
    http::BodyRequest body;
    body.method = std::move(request.method);
    body.body = std::move(request.body);
    body.timeout = std::chrono::seconds{10};
    body.max_bytes = 1024 * 1024;
    body.headers.push_back({"Content-Type", "application/json"});
    switch (conversation.platform) {
      case channel::Platform::telegram:
        body.url = "https://api.telegram.org/bot" + *token + request.path;
        break;
      case channel::Platform::qq:
        body.url = "https://api.sgroup.qq.com" + request.path;
        body.headers.push_back({"Authorization", "QQBot " + *token});
        body.headers.push_back({"X-Union-Appid", conversation.account});
        break;
      case channel::Platform::feishu:
        body.url = "https://open.feishu.cn" + request.path;
        body.headers.push_back({"Authorization", "Bearer " + *token});
        break;
    }
    auto response = co_await client.send(std::move(body));
    if (!response)
      co_return std::unexpected(core::Error{response.error().kind(), "channel HTTP transport failed"});
    channel::Response result{response->status_code, std::move(response->body), {}};
    for (const auto& header : response->headers) {
      if (!std::ranges::equal(header.name, std::string_view{"retry-after"}, [](unsigned char left, char right) {
            return std::tolower(left) == right;
          }))
        continue;
      std::uint64_t seconds{};
      auto [end, error] = std::from_chars(header.value.data(), header.value.data() + header.value.size(), seconds);
      if (error == std::errc{} && end == header.value.data() + header.value.size())
        result.retry_after = std::chrono::milliseconds{std::min(seconds, std::uint64_t{86400}) * 1000};
    }
    co_return result;
  } catch (...) { /* Credential and transport errors must not echo secrets. */
  }
  cancelled = co_await asio::this_coro::cancellation_state;
  co_return std::unexpected(cancelled.cancelled() != asio::cancellation_type::none
                                ? core::Error::cancelled()
                                : core::Error::internal("channel HTTP binding failed"));
}
}  // namespace
channel::Transport channel_http_transport(http::Client& client, ChannelCredential credential) {
  return [&client, credential = std::move(credential)](channel::Conversation conversation, channel::Request request) {
    return send_channel(client, credential, std::move(conversation), std::move(request));
  };
}
}  // namespace orangutan::bootstrap
