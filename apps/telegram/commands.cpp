#include "commands.hpp"

#include <algorithm>
#include <array>
#include <format>

#include <nlohmann/json.hpp>
#include <oran/bootstrap/session_status.hpp>

namespace orangutan::telegram_host {
using Json = nlohmann::json;
namespace {
constexpr std::array catalogue{
    std::pair{"new", "开启新会话，保留旧记录和长期记忆"},
    std::pair{"status", "查看当前会话、模型和最近用量"},
    std::pair{"tasks", "查看当前会话的后台任务"},
    std::pair{"stop", "停止当前会话的后台任务"},
    std::pair{"help", "查看命令和使用说明"},
    std::pair{"whoami", "查看自己的 Telegram 用户和聊天 ID"},
};

std::string lowercase(std::string_view text) {
  std::string value{text};
  std::ranges::transform(value, value.begin(), [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
  });
  return value;
}
}  // namespace

std::optional<Command> parse_command(std::string_view text, std::string_view bot_username) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos || text[first] != '/')
    return std::nullopt;
  text.remove_prefix(first + 1);
  const auto end = text.find_first_of(" \t\r\n");
  auto token = text.substr(0, end);
  auto target = token.find('@');
  const auto name = token.substr(0, target);
  if (name.empty() || name.size() > 32 || !std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      }))
    return std::nullopt;
  return Command{lowercase(name),
                 end != std::string_view::npos && text.find_first_not_of(" \t\r\n", end) != std::string_view::npos,
                 target == std::string_view::npos || lowercase(token.substr(target + 1)) == lowercase(bot_username)};
}

Json command_menu(std::string_view user) {
  auto commands = Json::array();
  for (const auto& [name, description] : catalogue)
    commands.push_back({{"command", name}, {"description", description}});
  return {{"scope", {{"type", "chat"}, {"chat_id", user}}}, {"commands", std::move(commands)}};
}

std::string command_help() {
  std::string text = "可以直接发送文字、图片，或回复并引用已有消息。\n\n";
  for (const auto& [name, description] : catalogue)
    text += std::format("/{} — {}\n", name, description);
  text += "\n/start 和 /commands 也可查看帮助。命令单独发送，不需要调用模型。";
  return text;
}

std::string format_session_status(const bootstrap::SessionStatus& status, std::string_view model) {
  std::string text =
      std::format("配置模型：`{}`\n长期记忆：{}\n", model, status.longterm_memory_enabled ? "开启" : "关闭");
  if (status.saved_messages) {
    text += std::format("已保存消息：{} 条\n摘要已覆盖：{} 条\n", *status.saved_messages, status.summarized_messages);
  } else {
    text += "会话存储：关闭\n";
  }
  if (status.last_turn) {
    const auto& last = *status.last_turn;
    text += std::format("最近调用模型：`{}`\n最近调用 token：输入 {} / 输出 {}\n缓存读取：{} token",
                        last.model,
                        last.input_tokens,
                        last.output_tokens,
                        last.cache_read_tokens);
  } else {
    text += status.trace_enabled ? "最近调用：暂无记录" : "最近调用：追踪已关闭";
  }
  return text;
}
}  // namespace orangutan::telegram_host
