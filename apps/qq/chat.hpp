#pragma once

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <oran/agent/prompt.hpp>
#include <oran/channel/dispatcher.hpp>
#include <oran/http/client.hpp>

namespace orangutan::qq_chat {
enum class Command {
  none,
  help,
  status,
  new_session,
  unknown
};
[[nodiscard]] Command command(const channel::Message& message);
inline constexpr std::size_t image_max_bytes = 5 * 1024 * 1024;
using Download = std::function<async::Awaitable<core::Result<http::BodyResponse>>(http::BodyRequest)>;
/// Resolve only against the host's already-bound private conversation journal.
[[nodiscard]] channel::Message resolve_reference(channel::Message message, const nlohmann::json& delivered);
[[nodiscard]] nlohmann::json saved_message(const channel::Message& message);
[[nodiscard]] async::Awaitable<core::Result<agent::PromptRequest>> prepare_prompt(const channel::Message& message,
                                                                                  const Download& download,
                                                                                  hook::Bus& hooks,
                                                                                  const permission::RuleSet& rules);
[[nodiscard]] std::string help();
[[nodiscard]] core::Result<std::vector<std::string>> split_markdown(std::string_view text, std::size_t limit);
}  // namespace orangutan::qq_chat
