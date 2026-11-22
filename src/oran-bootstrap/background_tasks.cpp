#include <oran/bootstrap/background_tasks.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <oran/async/channel.hpp>
#include <oran/async/sleep.hpp>
#include <oran/async/task_group.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config/config.hpp>
#include <oran/core/str.hpp>
#include <oran/permission/input_pattern.hpp>
#include <oran/tool/builtins.hpp>

#include "_impl/background-tasks.hpp"

namespace orangutan::bootstrap {
using core::Error;
using core::Result;

bool task_terminal(TaskState state) {
  return state == TaskState::succeeded || state == TaskState::failed || state == TaskState::cancelled;
}

Result<permission::RuleSet> copy_child_rules(std::span<const permission::Rule> rules) {
  permission::RuleSet copy;
  copy.reserve(rules.size());
  for (const auto& rule : rules) {
    permission::Rule value{.verdict = rule.verdict,
                           .tool_pattern = rule.tool_pattern,
                           .capability = rule.capability,
                           .replay_max = rule.replay_max,
                           .approval_ttl = rule.approval_ttl};
    if (rule.input_pattern) {
      auto pattern = permission::InputPattern::compile(std::string{rule.input_pattern->pattern()});
      if (!pattern)
        return std::unexpected(std::move(pattern).error());
      value.input_pattern = std::move(*pattern);
    }
    copy.push_back(std::move(value));
  }
  return copy;
}

BackgroundTasks::Impl::Impl(asio::any_io_executor exec,
                            RuntimeAssembly& resources,
                            const config::Config& cfg,
                            BackgroundTaskOptions opts,
                            agent::ToolSchedulerOptions sched)
    : executor{std::move(exec)}, assembly{&resources}, config{&cfg}, options{opts},
      scheduler{executor, registry, sched}, changed{executor, 1} {}

bool BackgroundTasks::Impl::has_record_capacity(bool reserved) {
  prune();
  std::size_t waiting = 0;
  for (const auto& team : teams)
    waiting += static_cast<std::size_t>(std::ranges::count_if(team->work, [](const auto& work) {
      return work.snapshot.state == TeamTaskState::waiting;
    }));
  if (reserved && waiting > 0)
    --waiting;
  return !closed && jobs.size() + waiting < options.max_records;
}

bool BackgroundTasks::Impl::has_capacity(bool reserved) {
  const bool records = has_record_capacity(reserved);
  const auto unfinished =
      std::ranges::count_if(jobs, [](const auto& job) { return !task_terminal(job->snapshot.state); });
  return records && static_cast<std::size_t>(unfinished) < options.max_running + options.max_queued;
}

Result<void> BackgroundTasks::Impl::enqueue(const std::shared_ptr<Job>& job) {
  if (!has_capacity(job->team != nullptr))
    return std::unexpected(Error{core::ErrorKind::mailbox_overflowed, "background task capacity reached"});
  if (std::ranges::any_of(jobs,
                          [&job](const auto& existing) { return existing->snapshot.task_id == job->snapshot.task_id; }))
    return std::unexpected(Error{core::ErrorKind::conflict, "background task identity collision"});
  jobs.push_back(job);
  job->deadline.expires_after(options.timeout);
  auto self = shared_from_this();
  job->deadline.async_wait([weak = std::weak_ptr{self}, weak_job = std::weak_ptr{job}](const asio::error_code& error) {
    if (error)
      return;
    if (auto state = weak.lock())
      if (auto pending = weak_job.lock()) {
        state->cancel_job(pending, true);
        state->pump();
      }
  });
  asio::post(executor, [self] { self->pump(); });
  return {};
}

void BackgroundTasks::Impl::prune() {
  const auto now = std::chrono::steady_clock::now();
  std::erase_if(jobs, [this, now](const auto& job) {
    return task_terminal(job->snapshot.state) && !job->claimed &&
           (!options.automatic_delivery || job->snapshot.acknowledged) && now - job->finished >= options.retention;
  });
}

std::shared_ptr<BackgroundTasks::Impl::Job> BackgroundTasks::Impl::find(const TaskOwner& owner, std::string_view id) {
  prune();
  const auto found = std::ranges::find_if(jobs, [&owner, id](const auto& job) {
    return job->owner == owner && job->snapshot.task_id == id;
  });
  return found == jobs.end() ? nullptr : *found;
}

void BackgroundTasks::Impl::cancel_job(const std::shared_ptr<Job>& job, bool timeout) {
  if (task_terminal(job->snapshot.state) || job->snapshot.state == TaskState::cancelling) {
    if (!timeout)
      job->snapshot.acknowledged = true;
    return;
  }
  job->timed_out = job->timed_out || timeout;
  if (!timeout)
    job->snapshot.acknowledged = true;
  job->snapshot.updated_at = core::time::now_utc();
  if (job->snapshot.state == TaskState::queued) {
    job->snapshot.state = timeout ? TaskState::failed : TaskState::cancelled;
    job->snapshot.error_kind = timeout ? core::ErrorKind::timeout : core::ErrorKind::cancelled;
    job->finished = std::chrono::steady_clock::now();
    job->deadline.cancel();
    job->child.reset();
    update_team_job(*job);
    job->team.reset();
    job->prompt.clear();
  } else {
    job->snapshot.state = TaskState::cancelling;
    update_team_job(*job);
    job->group->request_stop();
  }
  static_cast<void>(changed.try_send(0));
}

async::Awaitable<Result<void>> BackgroundTasks::Impl::execute(std::shared_ptr<Job> job) {
  job->outcome =
      co_await job->child->session->run_prompt({.prompt = std::move(job->prompt), .parent_turn_id = job->parent_turn});
  co_return Result<void>{};
}

async::Awaitable<void> BackgroundTasks::Impl::monitor(std::shared_ptr<Impl> self, std::shared_ptr<Job> job) {
  std::optional<Result<async::TaskGroupReport>> report;
  try {
    report = co_await job->group->join();
  } catch (...) {
    job->group->request_stop();
  }
  // Even an exceptional join cannot retire borrowed services before the
  // child finishes. This fallback is only for a failed join boundary.
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  while (job->group->active_tasks() != 0) {
    auto waited = co_await async::sleep_for(self->executor, std::chrono::milliseconds{1});
    if (!waited)
      job->group->request_stop();
  }
  job->deadline.cancel();
  try {
    if (job->snapshot.state == TaskState::cancelling) {
      job->snapshot.state = job->timed_out ? TaskState::failed : TaskState::cancelled;
      job->snapshot.error_kind = job->timed_out ? core::ErrorKind::timeout : core::ErrorKind::cancelled;
    } else if (!report || !*report || !(**report).all_succeeded() || !job->outcome || !*job->outcome) {
      const auto kind = job->outcome && !*job->outcome ? job->outcome->error().kind() : core::ErrorKind::internal;
      job->snapshot.state = kind == core::ErrorKind::cancelled ? TaskState::cancelled : TaskState::failed;
      job->snapshot.error_kind = kind;
    } else {
      job->snapshot.state = TaskState::succeeded;
      const auto& text = (*job->outcome)->text;
      job->result = core::str::truncate_to_code_point(text, self->options.max_result_bytes);
      job->snapshot.result_truncated = job->result.size() < text.size();
    }
  } catch (...) {
    job->snapshot.state = TaskState::failed;
    job->snapshot.error_kind = core::ErrorKind::internal;
  }
  job->snapshot.updated_at = core::time::now_utc();
  job->snapshot.result_bytes = job->result.size();
  job->finished = std::chrono::steady_clock::now();
  job->outcome.reset();
  self->update_team_job(*job);
  job->team.reset();
  job->child.reset();
  job->group.reset();
  --self->active;
  static_cast<void>(self->changed.try_send(0));
}

void BackgroundTasks::Impl::pump() {
  if (closed)
    return;
  pump_teams();
  for (const auto& job : jobs) {
    if (active >= options.max_running)
      break;
    if (job->snapshot.state != TaskState::queued)
      continue;
    auto group = async::TaskGroup::create(executor, {.max_tasks = 1, .max_completed = 1});
    if (!group) {
      cancel_job(job);
      continue;
    }
    job->group.emplace(std::move(*group));
    job->snapshot.state = TaskState::running;
    job->snapshot.updated_at = core::time::now_utc();
    update_team_job(*job);
    ++active;
    auto started = job->group->spawn(job->snapshot.task_id, [job] { return execute(job); });
    if (!started) {
      job->outcome = std::unexpected(std::move(started).error());
    }
    auto self = shared_from_this();
    asio::co_spawn(executor, monitor(self, job), [self, job](std::exception_ptr error) {
      // Never publish terminal state or decrement active twice on an
      // exceptional monitor exit. Keep ownership and rejoin before release.
      if (error && job->group) {
        job->group->request_stop();
        asio::co_spawn(self->executor, monitor(self, job), [self](std::exception_ptr) { self->pump(); });
      } else {
        self->pump();
      }
    });
  }
}

TaskOwner task_owner(const AgentSessionOptions& options) {
  return {options.session_id, options.scope_key, options.agent_key, options.identity};
}

Result<std::unique_ptr<BackgroundTasks>> BackgroundTasks::create(asio::any_io_executor executor,
                                                                 RuntimeAssembly& assembly,
                                                                 const config::Config& config,
                                                                 BackgroundTaskOptions options) {
  if (!executor || options.max_running == 0 || options.max_running > 32 || options.max_queued > 128 ||
      options.max_records < options.max_running || options.max_records > 1024 || options.max_result_bytes < 4 ||
      options.max_result_bytes > 4194304 || options.timeout <= std::chrono::milliseconds::zero() ||
      options.timeout > std::chrono::hours{24} || options.retention <= std::chrono::milliseconds::zero() ||
      options.retention > std::chrono::hours{24})
    return std::unexpected(Error::invalid_argument("invalid background task bounds or executor"));
  auto sched = scheduler_options_from(config);
  if (!sched)
    return std::unexpected(std::move(sched).error());
  auto impl = std::make_shared<Impl>(executor, assembly, config, options, *sched);
  if (auto added = tool::register_builtins(impl->registry); !added)
    return std::unexpected(std::move(added).error());
  if (assembly.longterm_memory_backend()) {
    if (auto added = tool::register_memory_tools(impl->registry); !added)
      return std::unexpected(std::move(added).error());
  }
  if (auto added = tool::register_agent_run(impl->registry, true); !added)
    return std::unexpected(std::move(added).error());
  if (auto added = tool::register_task_tools(impl->registry); !added)
    return std::unexpected(std::move(added).error());
  if (options.enable_teams) {
    if (auto added = tool::register_team_tools(impl->registry); !added)
      return std::unexpected(std::move(added).error());
  }
  return std::make_unique<BackgroundTasks>(std::move(impl), PrivateTag{});
}

BackgroundTasks::BackgroundTasks(std::shared_ptr<Impl> impl, PrivateTag) : impl_{std::move(impl)} {}
BackgroundTasks::~BackgroundTasks() {
  impl_->closed = true;
  for (const auto& team : impl_->teams)
    impl_->stop_team(*team);
  for (const auto& job : impl_->jobs)
    impl_->cancel_job(job);
}
bool BackgroundTasks::matches(const AgentSessionOptions& parent) const {
  return parent.config == impl_->config && parent.assembly == impl_->assembly && parent.executor == impl_->executor;
}
tool::Registry& BackgroundTasks::registry() {
  return impl_->registry;
}
agent::ToolScheduler& BackgroundTasks::scheduler() {
  return impl_->scheduler;
}

Result<TaskSnapshot> BackgroundTasks::start(const AgentSessionOptions& parent,
                                            tool::AgentRunRequest request,
                                            const tool::DispatchContext& context) {
  if (!matches(parent) || parent.parent_policy || parent.max_child_runs == 0)
    return std::unexpected(Error::permission_denied("background delegation is disabled for this caller"));
  if (request.prompt.empty() || request.prompt.size() > 16384 || !core::str::is_valid_utf8(request.prompt) ||
      request.label.size() > 120 || !core::str::is_valid_utf8(request.label) ||
      std::ranges::any_of(request.label, [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    return std::unexpected(Error::invalid_argument("invalid background task prompt or label"));
  impl_->prune();
  const auto unfinished =
      std::ranges::count_if(impl_->jobs, [](const auto& job) { return !task_terminal(job->snapshot.state); });
  if (impl_->closed || impl_->jobs.size() >= impl_->options.max_records ||
      static_cast<std::size_t>(unfinished) >= impl_->options.max_running + impl_->options.max_queued)
    return std::unexpected(Error{core::ErrorKind::mailbox_overflowed, "background task capacity reached"});
  auto id = core::generate_turn_id();
  if (!id)
    return std::unexpected(std::move(id).error());
  const auto text_id = core::format_turn_id_hex(*id);
  if (std::ranges::any_of(impl_->jobs, [&text_id](const auto& job) { return job->snapshot.task_id == text_id; }))
    return std::unexpected(Error{core::ErrorKind::conflict, "background task identity collision"});
  auto rules = copy_child_rules(context.rules);
  if (!rules)
    return std::unexpected(std::move(rules).error());
  auto job = std::make_shared<Impl::Job>(impl_->executor);
  job->child = std::make_shared<Impl::Child>();
  job->owner = task_owner(parent);
  job->parent_turn = context.parent_turn_id;
  job->child->parent_rules = std::move(*rules);
  job->snapshot.task_id = text_id;
  job->snapshot.agent_key = "child/" + text_id;
  job->snapshot.label = request.label.empty() ? "Background task" : request.label;
  job->snapshot.created_at = job->snapshot.updated_at = core::time::now_utc();
  job->snapshot.automatic_delivery = impl_->options.automatic_delivery;
  job->prompt = std::move(request.prompt);
  auto child_options = parent;
  child_options.session_id = *id;
  child_options.agent_key = job->snapshot.agent_key;
  child_options.identity = "child/" + text_id;
  child_options.origin = "background_agent";
  child_options.parent_turn_id = context.parent_turn_id;
  child_options.parent_policy = permission::PolicyView{.rules = job->child->parent_rules, .mode = context.mode};
  child_options.max_child_runs = 0;
  child_options.per_agent_overlay.clear();
  child_options.trace_context_json = "{}";
  child_options.event_sink = nullptr;
  child_options.registry = &impl_->registry;
  child_options.scheduler = &impl_->scheduler;
  child_options.background_tasks = nullptr;
  child_options.inbox.reset();
  child_options.team_send = {};
  auto child = AgentSession::create(std::move(child_options));
  if (!child)
    return std::unexpected(std::move(child).error());
  job->child->session = std::move(*child);
  auto admitted = impl_->enqueue(job);
  if (!admitted)
    return std::unexpected(std::move(admitted).error());
  return job->snapshot;
}

Result<TaskSnapshot>
BackgroundTasks::get(const TaskOwner& owner, std::string_view id, std::size_t offset, std::size_t max_bytes) {
  auto job = impl_->find(owner, id);
  if (!job)
    return std::unexpected(Error::not_found("task is unavailable in this session or has expired"));
  if (max_bytes == 0 || max_bytes > 16384 || offset > job->result.size() ||
      (offset < job->result.size() && (static_cast<unsigned char>(job->result[offset]) & 0xc0U) == 0x80U))
    return std::unexpected(Error::invalid_argument("invalid task result window"));
  auto result = job->snapshot;
  result.result_offset = offset;
  result.result = core::str::truncate_to_code_point(std::string_view{job->result}.substr(offset), max_bytes);
  if (result.result.empty() && offset < job->result.size())
    return std::unexpected(Error::invalid_argument("task result window cannot fit one UTF-8 character"));
  if (offset + result.result.size() < job->result.size())
    result.next_offset = offset + result.result.size();
  return result;
}
Result<TaskSnapshot> BackgroundTasks::cancel(const TaskOwner& owner, std::string_view id) {
  auto job = impl_->find(owner, id);
  if (!job)
    return std::unexpected(Error::not_found("task is unavailable in this session or has expired"));
  impl_->cancel_job(job);
  return job->snapshot;
}
std::vector<TaskSnapshot> BackgroundTasks::list(const TaskOwner& owner) {
  impl_->prune();
  std::vector<TaskSnapshot> result;
  for (const auto& job : impl_->jobs)
    if (job->owner == owner)
      result.push_back(job->snapshot);
  return result;
}
std::optional<std::string> BackgroundTasks::next_completion(const TaskOwner& owner) {
  for (const auto& job : impl_->jobs)
    if (job->owner == owner && task_terminal(job->snapshot.state) && job->snapshot.automatic_delivery &&
        !job->snapshot.acknowledged && !job->claimed)
      return job->snapshot.task_id;
  return std::nullopt;
}
Result<bool> BackgroundTasks::claim_completion(const TaskOwner& owner, std::string_view id) {
  auto job = impl_->find(owner, id);
  if (!job)
    return std::unexpected(Error::not_found("task completion is unavailable"));
  if (job->snapshot.acknowledged)
    return false;
  if (!task_terminal(job->snapshot.state) || job->claimed)
    return std::unexpected(Error{core::ErrorKind::conflict, "task completion is not ready"});
  job->claimed = true;
  return true;
}
void BackgroundTasks::finish_completion(const TaskOwner& owner, std::string_view id, bool committed) {
  if (auto job = impl_->find(owner, id)) {
    job->claimed = false;
    job->snapshot.acknowledged = job->snapshot.acknowledged || committed;
  }
}
void BackgroundTasks::cancel_owner(const TaskOwner& owner, std::optional<core::TurnId> turn) {
  if (auto team = impl_->find_team(owner)) {
    if (!turn) {
      impl_->stop_team(*team);
    } else {
      for (auto& work : team->work)
        if (work.parent_turn == turn && work.snapshot.state == TeamTaskState::waiting)
          work.snapshot.state = TeamTaskState::cancelled;
    }
  }
  for (const auto& job : impl_->jobs)
    if (job->owner == owner && (!turn || job->parent_turn == turn))
      impl_->cancel_job(job);
}
async::Awaitable<Result<void>> BackgroundTasks::shutdown() {
  auto self = impl_;
  if (self->joining)
    co_return std::unexpected(Error{core::ErrorKind::conflict, "background shutdown already active"});
  self->joining = true;
  self->closed = true;
  for (const auto& team : self->teams)
    self->stop_team(*team);
  for (const auto& job : self->jobs)
    self->cancel_job(job);
  co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  while (self->active != 0) {
    auto changed = co_await self->changed.receive();
    if (!changed)
      co_return std::unexpected(std::move(changed).error());
  }
  self->joining = false;
  co_return Result<void>{};
}
}  // namespace orangutan::bootstrap
