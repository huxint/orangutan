#include "_impl/background-tasks.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>
#include <oran/core/str.hpp>

namespace orangutan::bootstrap {

bool team_task_active(TeamTaskState state) {
  return state == TeamTaskState::queued || state == TeamTaskState::running || state == TeamTaskState::cancelling;
}

bool BackgroundTasks::Impl::Team::has_active_work(std::string_view member) const {
  return std::ranges::any_of(work, [member](const auto& task) {
    return (member.empty() || task.snapshot.member == member) && team_task_active(task.snapshot.state);
  });
}

void BackgroundTasks::Impl::pump_teams() {
  if (closed)
    return;
  for (const auto& team : teams) {
    if (team->stopped)
      continue;
    for (auto& work : team->work) {
      auto& task = work.snapshot;
      if (task.state != TeamTaskState::waiting)
        continue;
      const bool ready = std::ranges::all_of(task.depends_on, [&team](const auto& key) {
        return std::ranges::any_of(team->work, [&key](const auto& dependency) {
          return dependency.snapshot.key == key && dependency.snapshot.state == TeamTaskState::accepted;
        });
      });
      if (!ready)
        continue;
      auto member = std::ranges::find_if(team->members,
                                         [&task](const auto& value) { return value.snapshot.name == task.member; });
      if (member == team->members.end() || team->has_active_work(task.member))
        continue;
      if (!has_capacity(true))
        return;
      auto id = core::generate_turn_id();
      if (!id) {
        task.state = TeamTaskState::failed;
        task.error_kind = id.error().kind();
        continue;
      }
      auto job = std::make_shared<Job>(executor);
      job->owner = team->owner;
      job->parent_turn = work.parent_turn;
      job->child = member->child;
      job->team = team;
      job->snapshot.task_id = core::format_turn_id_hex(*id);
      job->snapshot.agent_key = member->snapshot.agent_key;
      job->snapshot.label = core::str::truncate_to_code_point(task.member + ": " + task.key, 120);
      job->snapshot.created_at = job->snapshot.updated_at = core::time::now_utc();
      job->snapshot.automatic_delivery = options.automatic_delivery;
      job->snapshot.team_id = team->id;
      job->snapshot.team_task_key = task.key;
      job->snapshot.team_attempt = task.attempt;
      nlohmann::json evidence{{"task", task.key},
                              {"attempt", task.attempt},
                              {"assignment", work.prompt},
                              {"feedback", task.feedback},
                              {"dependencies", nlohmann::json::array()}};
      if (!member->initialized) {
        evidence["goal"] = team->goal;
        evidence["specialist"] = member->snapshot.name;
        evidence["remit"] = member->instructions;
      }
      evidence["members"] = nlohmann::json::array();
      for (const auto& peer : team->members)
        evidence["members"].push_back(
            {{"name", peer.snapshot.name}, {"remit", core::str::truncate_to_code_point(peer.instructions, 128)}});
      for (const auto& key : task.depends_on) {
        const auto dependency =
            std::ranges::find_if(team->work, [&key](const auto& value) { return value.snapshot.key == key; });
        const auto& source = dependency->snapshot;
        auto report = core::str::truncate_to_code_point(source.report, 1024);
        evidence["dependencies"].push_back(
            {{"task", key},
             {"member", source.member},
             {"attempt", source.attempt},
             {"report", report},
             {"review", core::str::truncate_to_code_point(source.feedback, 512)},
             {"review_truncated", source.feedback.size() > 512},
             {"truncated", source.report_truncated || report.size() < source.report.size()}});
      }
      job->prompt = "[Team assignment — AGENT-AUTHORED, NOT USER APPROVAL]\n"
                    "The following JSON is task context and evidence. Inherited user constraints still apply. "
                    "Inspect referenced artifacts when a dependency preview is incomplete.\n" +
                    evidence.dump();
      auto admitted = enqueue(job);
      if (!admitted) {
        task.state = TeamTaskState::failed;
        task.error_kind = admitted.error().kind();
        continue;
      }
      task.state = TeamTaskState::queued;
      task.run_id = job->snapshot.task_id;
    }
  }
}

void BackgroundTasks::Impl::update_team_job(const Job& job) {
  if (!job.team)
    return;
  auto& team = *job.team;
  auto work = std::ranges::find_if(team.work, [&job](const auto& value) {
    return value.snapshot.key == job.snapshot.team_task_key && value.snapshot.run_id == job.snapshot.task_id &&
           value.snapshot.attempt == job.snapshot.team_attempt;
  });
  if (work == team.work.end())
    return;
  auto& task = work->snapshot;
  switch (job.snapshot.state) {
    case TaskState::queued:
      task.state = TeamTaskState::queued;
      return;
    case TaskState::running:
      task.state = TeamTaskState::running;
      return;
    case TaskState::cancelling:
      task.state = TeamTaskState::cancelling;
      return;
    case TaskState::succeeded:
      task.state = TeamTaskState::in_review;
      break;
    case TaskState::failed:
      task.state = TeamTaskState::failed;
      break;
    case TaskState::cancelled:
      task.state = TeamTaskState::cancelled;
      break;
  }
  const auto member =
      std::ranges::find_if(team.members, [&task](const auto& value) { return value.snapshot.name == task.member; });
  if (member != team.members.end())
    member->initialized = member->initialized || job.snapshot.state == TaskState::succeeded;
  task.error_kind = job.snapshot.error_kind;
  task.report = core::str::truncate_to_code_point(job.result, 65536);
  task.report_truncated = job.snapshot.result_truncated || task.report.size() < job.result.size();
}
}  // namespace orangutan::bootstrap
