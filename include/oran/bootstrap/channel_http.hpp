#pragma once

#include <functional>
#include <string>

#include <oran/channel/dispatcher.hpp>

namespace orangutan::http {
class Client;
}

namespace orangutan::bootstrap {
/// Resolve a current Telegram bot token, QQ access token, or Feishu tenant access
/// token. The host owns refresh/caching and keeps credential values out of logs.
using ChannelCredential = std::function<async::Awaitable<core::Result<std::string>>(channel::Conversation)>;

/// Borrow client until all dispatcher calls finish. Uses fixed official endpoints,
/// bounded requests, current credentials per send, and sanitized transport errors.
[[nodiscard]] channel::Transport channel_http_transport(http::Client& client, ChannelCredential credential);
}  // namespace orangutan::bootstrap
