#include "_impl/background-tasks.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/str.hpp>

namespace orangutan::bootstrap {
namespace {
using Json = nlohmann::json;
Json task_value(const TeamTaskSnapshot& task) {
  return {{"key", task.key},
          {"member", task.member},
          {"attempt", task.attempt},
          {"state", core::enum_name(task.state)},
          {"depends_on", task.depends_on},
          {"run_id", task.run_id},
          {"report_bytes", task.report.size()},
          {"report_truncated", task.report_truncated},
          {"error_kind", task.error_kind ? Json(core::enum_name(*task.error_kind)) : Json(nullptr)}};
}
}  // namespace

core::Result<tool::Output>
team_output(const BackgroundTasks::Impl::Team& team, bool automatic_delivery, tool::TeamGetRequest request) {
  Json data{{"kind", "agent_team"},
            {"team_id", team.id},
            {"stopped", team.stopped},
            {"completion_delivery", automatic_delivery ? "automatic" : "query_only"},
            {"survives_restart", false},
            {"sent_messages", team.messages.size()},
            {"pending_messages", team.pending_messages()},
            {"tasks", Json::array()}};
  if (request.key.empty()) {
    data["members"] = Json::array();
    for (const auto& member : team.members)
      data["members"].push_back({{"name", member.snapshot.name}, {"tools", member.snapshot.tools}});
    for (const auto& work : team.work)
      data["tasks"].push_back(task_value(work.snapshot));
  } else {
    const auto work =
        std::ranges::find_if(team.work, [&request](const auto& value) { return value.snapshot.key == request.key; });
    if (work == team.work.end())
      return std::unexpected(core::Error::not_found("team task is unavailable"));
    const auto* task = &work->snapshot;
    if (request.offset > task->report.size() ||
        (request.offset < task->report.size() &&
         (static_cast<unsigned char>(task->report[request.offset]) & 0xc0U) == 0x80U))
      return std::unexpected(core::Error::invalid_argument("invalid team report byte offset"));
    auto report =
        core::str::truncate_to_code_point(std::string_view{task->report}.substr(request.offset), request.max_bytes);
    if (report.empty() && request.offset < task->report.size())
      return std::unexpected(core::Error::invalid_argument("team report window cannot fit one UTF-8 character"));
    auto value = task_value(*task);
    value["report"] = report;
    value["feedback"] = task->feedback;
    value["offset"] = request.offset;
    const auto next = request.offset + report.size();
    value["next_offset"] = next < task->report.size() ? Json(next) : Json(nullptr);
    data["tasks"].push_back(std::move(value));
  }
  auto encoded = data.dump();
  auto text = encoded;
  text += "\nA reported task awaits your review. Only accepting its current attempt unlocks dependents. "
          "Waiting can mean unaccepted dependencies, a busy member, or shared task capacity. "
          "Use TeamGet with a task key for evidence; capped reports are incomplete.";
  text += automatic_delivery ? " Completion arrives automatically; do not poll just to wait."
                             : " This host does not send automatic completion; inspect when needed.";
  return tool::Output{.text = std::move(text), .data_json = std::move(encoded)};
}
}  // namespace orangutan::bootstrap
