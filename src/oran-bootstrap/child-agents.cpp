#include "_impl/child-agents.hpp"

#include <utility>

#include <asio/this_coro.hpp>
#include <nlohmann/json.hpp>

#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/background_tasks.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bootstrap {
namespace {

async::Awaitable<core::Result<tool::Output>> run_child_agent(tool::AgentRunRequest request,
                                                             const AgentSessionOptions& parent,
                                                             tool::Registry& registry,
                                                             agent::ToolScheduler& scheduler,
                                                             std::size_t& child_runs,
                                                             tool::DispatchContext& context) {
  if (auto cancellation = co_await asio::this_coro::cancellation_state;
      cancellation.cancelled() != asio::cancellation_type::none) {
    co_return std::unexpected(core::Error::cancelled());
  }
  if (child_runs >= parent.max_child_runs) {
    co_return std::unexpected(
        core::Error{core::ErrorKind::mailbox_overflowed, "AgentRun: child admission limit reached"}
            .with("reason", "child_limit")
            .with("limit", std::to_string(parent.max_child_runs)));
  }
  if (request.background) {
    if (!parent.background_tasks)
      co_return std::unexpected(core::Error::permission_denied("background tasks are unavailable in this host"));
    auto started = parent.background_tasks->start(parent, std::move(request), context);
    if (!started)
      co_return std::unexpected(std::move(started).error());
    ++child_runs;
    co_return task_output(*started);
  }
  auto session_id = core::generate_turn_id();
  if (!session_id) {
    co_return std::unexpected(std::move(session_id).error());
  }
  const auto session_id_text = core::format_turn_id_hex(*session_id);
  auto options = parent;
  options.session_id = *session_id;
  options.agent_key = "child/" + session_id_text;
  options.identity = "child/" + session_id_text;
  options.origin = "child_agent";
  options.parent_turn_id = context.parent_turn_id;
  options.parent_policy = permission::PolicyView{.rules = context.rules, .mode = context.mode};
  options.max_child_runs = 0;
  options.per_agent_overlay.clear();
  options.trace_context_json = "{}";
  options.event_sink = nullptr;
  options.background_tasks = nullptr;
  options.inbox.reset();
  options.team_send = {};
  options.registry = &registry;
  options.scheduler = &scheduler;
  auto child = AgentSession::create(std::move(options));
  if (!child) {
    co_return std::unexpected(std::move(child).error());
  }
  ++child_runs;
  auto result = co_await (*child)->run_prompt(agent::PromptRequest{.prompt = std::move(request.prompt)});
  if (!result) {
    co_return std::unexpected(std::move(result).error().with("agent_key", "child/" + session_id_text));
  }
  co_return tool::Output{
      .text = std::move(result->text),
      .data_json = nlohmann::json{{"kind", "agent_run"},
                                  {"agent_key", "child/" + session_id_text},
                                  {"session_id", session_id_text}}
                       .dump(),
  };
}

}  // namespace

void bind_task_tools(tool::DispatchContext& context, const AgentSessionOptions& options) {
  if (options.team_send) {
    context.team = [&options](tool::TeamCommand command,
                              tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
      const auto* message = std::get_if<tool::TeamMessageRequest>(&command);
      if (!message)
        co_return std::unexpected(core::Error::permission_denied("only the leader can manage team tasks"));
      co_return options.team_send(*message);
    };
  }
  if (!options.background_tasks || options.parent_policy)
    return;
  const auto owner = task_owner(options);
  auto* tasks = options.background_tasks;
  context.task_get = [tasks, owner](tool::TaskRequest request,
                                    tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    auto result = tasks->get(owner, request.task_id, request.offset, request.max_bytes);
    if (!result)
      co_return std::unexpected(std::move(result).error());
    co_return task_output(*result);
  };
  context.task_cancel = [tasks, owner](tool::TaskRequest request,
                                       tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    auto result = tasks->cancel(owner, request.task_id);
    if (!result)
      co_return std::unexpected(std::move(result).error());
    co_return task_output(*result);
  };
  if (tasks->teams_enabled() && options.max_child_runs > 0) {
    context.team = [tasks, &options](tool::TeamCommand command,
                                     tool::DispatchContext& caller) -> async::Awaitable<core::Result<tool::Output>> {
      co_return tasks->team_command(options, std::move(command), caller);
    };
  }
}

void bind_child_agents(tool::DispatchContext& context,
                       const AgentSessionOptions& options,
                       tool::Registry& registry,
                       agent::ToolScheduler& scheduler,
                       std::size_t& child_runs) {
  if (options.parent_policy || options.max_child_runs == 0) {
    return;
  }
  context.agent_run = [&options, &registry, &scheduler, &child_runs](tool::AgentRunRequest request,
                                                                     tool::DispatchContext& caller) {
    return run_child_agent(std::move(request), options, registry, scheduler, child_runs, caller);
  };
}

}  // namespace orangutan::bootstrap
