#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <oran/core/error.hpp>

namespace orangutan::bootstrap {

enum class TeamTaskState {
  waiting,
  queued,
  running,
  in_review,
  accepted,
  failed,
  cancelling,
  cancelled
};

struct TeamMemberSnapshot {
  std::string name{};
  std::string agent_key{};
  std::string session_id{};
  std::vector<std::string> tools{};
};

struct TeamTaskSnapshot {
  std::string key{};
  std::string member{};
  std::vector<std::string> depends_on{};
  std::size_t attempt{1};
  TeamTaskState state{TeamTaskState::waiting};
  std::string run_id{};
  std::string report{};
  bool report_truncated{false};
  std::string feedback{};
  std::optional<core::ErrorKind> error_kind{};
};

struct TeamSnapshot {
  std::string team_id{};
  std::string goal{};
  bool stopped{false};
  bool automatic_delivery{false};
  std::size_t sent_messages{0};
  std::size_t pending_messages{0};
  std::vector<TeamMemberSnapshot> members{};
  std::vector<TeamTaskSnapshot> tasks{};
};

}  // namespace orangutan::bootstrap
