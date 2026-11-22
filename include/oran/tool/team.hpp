#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <oran/core/result.hpp>

namespace orangutan::tool {

inline constexpr std::string_view TEAM_CREATE_NAME{"TeamCreate"};
inline constexpr std::string_view TEAM_TASK_CREATE_NAME{"TeamTaskCreate"};
inline constexpr std::string_view TEAM_TASK_UPDATE_NAME{"TeamTaskUpdate"};
inline constexpr std::string_view TEAM_GET_NAME{"TeamGet"};
inline constexpr std::string_view TEAM_STOP_NAME{"TeamStop"};
inline constexpr std::string_view TEAM_MESSAGE_NAME{"TeamMessage"};
inline constexpr std::string_view TEAM_MEMBER_ADD_NAME{"TeamMemberAdd"};

struct TeamMemberSpec {
  std::string name{};
  std::string instructions{};
  std::vector<std::string> tools{};
};

struct TeamCreateRequest {
  std::string goal{};
  std::vector<TeamMemberSpec> members{};
};
struct TeamMemberAddRequest {
  TeamMemberSpec member;
};

struct TeamTaskCreateRequest {
  std::string key{};
  std::string member{};
  std::string prompt{};
  std::vector<std::string> depends_on{};
};

enum class TeamTaskAction {
  accept,
  revise,
  cancel
};

struct TeamTaskUpdateRequest {
  std::string key{};
  std::size_t attempt{1};
  TeamTaskAction action{TeamTaskAction::accept};
  std::string feedback{};
  std::string team_id{};
};

struct TeamGetRequest {
  std::string key{};
  std::size_t offset{0};
  std::size_t max_bytes{2048};
};

struct TeamStopRequest {};
struct TeamMessageRequest {
  std::string recipient{};
  std::string task_key{};
  std::string message{};
};
using TeamCommand = std::variant<TeamCreateRequest,
                                 TeamTaskCreateRequest,
                                 TeamTaskUpdateRequest,
                                 TeamGetRequest,
                                 TeamStopRequest,
                                 TeamMessageRequest,
                                 TeamMemberAddRequest>;

[[nodiscard]] bool is_team_tool(std::string_view name);
/// Also validates trusted host calls before they enter the coordination state.
[[nodiscard]] core::Result<void> validate_team_command(const TeamCommand& command);

}  // namespace orangutan::tool
