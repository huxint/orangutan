#pragma once

#include <oran/agent/prompt.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/core/content.hpp>

namespace orangutan::telegram_host {
inline constexpr std::size_t image_max_bytes = 5 * 1024 * 1024;
using DownloadImage = std::function<async::Awaitable<core::Result<std::string>>(std::string)>;

/// Retrieve one Telegram image after admission. Only safe relative Telegram file
/// paths reach download; returned content has no credential or remote URL.
[[nodiscard]] async::Awaitable<core::Result<core::ImageContent>> load_image(const channel::Message& message,
                                                                            const channel::ImageAttachment& attachment,
                                                                            const channel::Transport& transport,
                                                                            const DownloadImage& download,
                                                                            hook::Bus& hooks,
                                                                            const permission::RuleSet& rules);

[[nodiscard]] async::Awaitable<core::Result<agent::PromptRequest>> prepare_prompt(const channel::Message& message,
                                                                                  const channel::Transport& transport,
                                                                                  const DownloadImage& download,
                                                                                  hook::Bus& hooks,
                                                                                  const permission::RuleSet& rules);
}  // namespace orangutan::telegram_host
