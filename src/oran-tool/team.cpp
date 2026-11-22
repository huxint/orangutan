#include <oran/tool/team.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

#include <oran/core/str.hpp>
#include <oran/core/turn_id.hpp>

namespace orangutan::tool {
namespace {
bool text_valid(std::string_view value, std::size_t maximum, bool empty = false) {
  return (empty || !value.empty()) && value.size() <= maximum && core::str::is_valid_utf8(value);
}
bool key_valid(std::string_view value) {
  return text_valid(value, 64) && std::ranges::none_of(value, [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
bool keys_valid(const std::vector<std::string>& keys, std::size_t maximum) {
  if (keys.size() > maximum || !std::ranges::all_of(keys, key_valid))
    return false;
  auto ordered = keys;
  std::ranges::sort(ordered);
  return std::ranges::adjacent_find(ordered) == ordered.end();
}
}  // namespace

bool is_team_tool(std::string_view name) {
  constexpr auto names = std::array{TEAM_CREATE_NAME,
                                    TEAM_TASK_CREATE_NAME,
                                    TEAM_TASK_UPDATE_NAME,
                                    TEAM_GET_NAME,
                                    TEAM_STOP_NAME,
                                    TEAM_MESSAGE_NAME,
                                    TEAM_MEMBER_ADD_NAME};
  return std::ranges::contains(names, name);
}

core::Result<void> validate_team_command(const TeamCommand& command) {
  const bool valid = std::visit(
      []<typename T>(const T& request) {
        if constexpr (std::is_same_v<T, TeamCreateRequest>) {
          if (!text_valid(request.goal, 8192) || request.members.empty() || request.members.size() > 4)
            return false;
          std::vector<std::string> names;
          for (const auto& member : request.members) {
            if (!key_valid(member.name) || member.name == "leader" || !text_valid(member.instructions, 4096) ||
                !keys_valid(member.tools, 32))
              return false;
            names.push_back(member.name);
          }
          return keys_valid(names, 4);
        } else if constexpr (std::is_same_v<T, TeamMemberAddRequest>) {
          return key_valid(request.member.name) && request.member.name != "leader" &&
                 text_valid(request.member.instructions, 4096) && keys_valid(request.member.tools, 32);
        } else if constexpr (std::is_same_v<T, TeamTaskCreateRequest>) {
          return key_valid(request.key) && key_valid(request.member) && text_valid(request.prompt, 8192) &&
                 keys_valid(request.depends_on, 32) && !std::ranges::contains(request.depends_on, request.key);
        } else if constexpr (std::is_same_v<T, TeamTaskUpdateRequest>) {
          const auto team = core::parse_turn_id_hex(request.team_id);
          return team && !core::is_zero_turn_id(*team) && key_valid(request.key) && request.attempt >= 1 &&
                 request.attempt <= 3 &&
                 (request.action == TeamTaskAction::accept || request.action == TeamTaskAction::revise ||
                  request.action == TeamTaskAction::cancel) &&
                 text_valid(request.feedback, 4096, request.action == TeamTaskAction::cancel);
        } else if constexpr (std::is_same_v<T, TeamGetRequest>) {
          return (request.key.empty() || key_valid(request.key)) && request.offset <= 65536 && request.max_bytes >= 1 &&
                 request.max_bytes <= 8192 && (!request.key.empty() || request.offset == 0);
        } else if constexpr (std::is_same_v<T, TeamMessageRequest>) {
          return key_valid(request.recipient) && key_valid(request.task_key) && text_valid(request.message, 4096);
        } else {
          return true;
        }
      },
      command);
  if (!valid)
    return std::unexpected(core::Error::invalid_argument("invalid team command, text, keys or bounds"));
  return {};
}
}  // namespace orangutan::tool
