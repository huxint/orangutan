#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace orangutan::bootstrap {
struct SessionStatus;
}

namespace orangutan::telegram_host {
struct Command {
  std::string name;
  bool has_arguments{false};
  bool addressed_here{true};
};

[[nodiscard]] std::optional<Command> parse_command(std::string_view text, std::string_view bot_username);
[[nodiscard]] nlohmann::json command_menu(std::string_view user);
[[nodiscard]] std::string command_help();

[[nodiscard]] std::string format_session_status(const bootstrap::SessionStatus& status, std::string_view model);
}  // namespace orangutan::telegram_host
