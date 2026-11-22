#include <oran/tool/registry.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/this_coro.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/hook/payload.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/approval_broker.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/path-locks.hpp>

#include "_impl/approval_resolution.hpp"
#include "_impl/audit_metadata.hpp"
#include "_impl/input_redaction.hpp"
#include "_impl/path-lock-table.hpp"
#include "_impl/path_resolution.hpp"
#include "_impl/schema_validation.hpp"

namespace orangutan::tool {

namespace {

[[nodiscard]] core::Error make_lookup_error(std::string_view name) {
  return core::Error::not_found("tool is not registered").with("tool", std::string{name});
}

[[nodiscard]] permission::AuditEvent build_event(std::string_view name,
                                                 std::string_view input_json,
                                                 const permission::Decision& decision,
                                                 const DispatchContext& ctx,
                                                 std::string metadata_json) {
  auto event = permission::make_audit_event_from_decision(decision);
  event.scope_key = ctx.scope_key;
  event.agent_key = ctx.agent_key;
  event.tool_name = std::string{name};
  event.identity = ctx.identity;
  event.input_hash = permission::ApprovalAuthority::input_hash(input_json);
  event.parent_turn_id = ctx.parent_turn_id;
  event.metadata_json = std::move(metadata_json);
  return event;
}

[[nodiscard]] std::string input_hash_hex(std::string_view input_json) {
  return permission::to_hex(permission::ApprovalAuthority::input_hash(input_json));
}

[[nodiscard]] std::string hook_decision_reason(const hook::HookDecision& decision, std::string_view fallback) {
  return decision.reason.empty() ? std::string{fallback} : decision.reason;
}

[[nodiscard]] permission::Decision hook_blocked_decision(std::string reason) {
  return permission::Decision{
      .verdict = permission::Verdict::deny,
      .reason = std::move(reason),
  };
}

[[nodiscard]] core::Error hook_veto_error(std::string_view name, std::string reason) {
  return core::Error::permission_denied("tool blocked by hook")
      .with("tool", std::string{name})
      .with("reason", "blocked_by_hook")
      .with("hook_reason", std::move(reason));
}

[[nodiscard]] core::Error hook_rewrite_error(std::string_view name, std::string reason) {
  return core::Error::permission_denied("tool rewrite rejected by hook")
      .with("tool", std::string{name})
      .with("reason", "blocked_by_hook")
      .with("hook_reason", std::move(reason));
}

[[nodiscard]] permission::Decision require_approval_decision(permission::Decision decision,
                                                             const hook::HookDecision& hook_decision) {
  if (decision.verdict != permission::Verdict::allow) {
    return decision;
  }
  decision.verdict = permission::Verdict::ask;
  decision.reason = hook_decision_reason(hook_decision, "hook require_approval");
  return decision;
}

[[nodiscard]] permission::Decision require_workspace_override_approval(permission::Decision decision) {
  if (decision.verdict != permission::Verdict::allow) {
    return decision;
  }
  decision.verdict = permission::Verdict::ask;
  decision.reason = "outside_workspace_override";
  decision.replay_max = 1;
  return decision;
}

[[nodiscard]] bool has_context(const core::Error& error, std::string_view key, std::string_view value) noexcept {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

[[nodiscard]] std::string error_kind_label(const core::Error& error) {
  if (has_context(error, "reason", "blocked_by_hook")) {
    return "blocked_by_hook";
  }
  return std::string{core::enum_name(error.kind())};
}

[[nodiscard]] hook::Identity make_hook_identity(const DispatchContext& ctx) {
  return hook::Identity{
      .scope_key = ctx.scope_key,
      .agent_key = ctx.agent_key,
      .identity = ctx.identity,
  };
}

[[nodiscard]] hook::ToolBeforePayload build_before_payload(std::string_view name,
                                                           std::string_view input_json,
                                                           const DispatchContext& ctx,
                                                           core::Time started_at) {
  return hook::ToolBeforePayload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .redacted_input_json = detail::redacted_hook_input_json(name, input_json),
      .who = make_hook_identity(ctx),
      .started_at = started_at,
  };
}

[[nodiscard]] hook::ToolAfterPayload build_after_payload(std::string_view name,
                                                         std::string_view input_json,
                                                         const DispatchContext& ctx,
                                                         core::Time started_at,
                                                         core::Time finished_at,
                                                         const core::Result<Output>& result) {
  auto usage = hook::ToolUsage{};
  if (result.has_value()) {
    usage = hook::ToolUsage{
        .bytes_read = result->usage.bytes_read,
        .bytes_written = result->usage.bytes_written,
        .files_touched = result->usage.files_touched,
        .match_count = result->usage.match_count,
        .cost_estimate = result->usage.cost_estimate,
        .wall_time = result->usage.wall_time,
        .truncated = result->usage.truncated,
        .data_dropped = result->usage.data_dropped,
    };
  }
  hook::ToolAfterPayload payload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .redacted_input_json = detail::redacted_hook_input_json(name, input_json),
      .who = make_hook_identity(ctx),
      .succeeded = result.has_value(),
      .output_text = result.has_value() ? result->text : std::string{},
      .data_json = result.has_value() ? result->data_json : std::nullopt,
      .usage = usage,
      .error_kind = result.has_value() ? std::string{} : error_kind_label(result.error()),
      .error_message = result.has_value() ? std::string{} : std::string{result.error().message()},
      .started_at = started_at,
      .finished_at = finished_at,
      .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at.to_system_time_point() -
                                                                       started_at.to_system_time_point()),
  };
  return payload;
}

[[nodiscard]] hook::PermissionAskRenderedPayload build_permission_ask_payload(std::string_view name,
                                                                              std::string_view input_json,
                                                                              const DispatchContext& ctx,
                                                                              const permission::Decision& decision) {
  return hook::PermissionAskRenderedPayload{
      .tool_name = std::string{name},
      .input_json = std::string{input_json},
      .redacted_input_json = detail::redacted_hook_input_json(name, input_json),
      .who = make_hook_identity(ctx),
      .decision_reason = decision.reason,
      .replay_max = decision.replay_max,
      .approval_ttl = decision.approval_ttl,
      .requested_at = ctx.now,
      .turn_id = ctx.parent_turn_id,
  };
}

}  // namespace

DispatchContext DispatchContext::for_now(asio::any_io_executor executor,
                                         std::span<const permission::Rule> rules,
                                         permission::AuditSink& audit,
                                         std::string scope_key,
                                         std::string agent_key,
                                         std::string identity) {
  return DispatchContext{
      .executor = std::move(executor),
      .rules = rules,
      .audit = audit,
      .now = core::time::now_utc(),
      .scope_key = std::move(scope_key),
      .agent_key = std::move(agent_key),
      .identity = std::move(identity),
  };
}

DispatchContext DispatchContext::for_now(const DispatchContext& prototype, bool thread_approval_token_output) {
  return DispatchContext{
      .executor = prototype.executor,
      .mode = prototype.mode,
      .rules = prototype.rules,
      .audit = prototype.audit,
      .parent_policy = prototype.parent_policy,
      .approval_broker = prototype.approval_broker,
      .approval_token = prototype.approval_token,
      .approval_token_output = thread_approval_token_output ? prototype.approval_token_output : nullptr,
      .now = core::time::now_utc(),
      .bus = prototype.bus,
      .memory_recall = prototype.memory_recall,
      .memory_remember = prototype.memory_remember,
      .memory_forget = prototype.memory_forget,
      .agent_run = prototype.agent_run,
      .task_get = prototype.task_get,
      .task_cancel = prototype.task_cancel,
      .team = prototype.team,
      .active_tools = prototype.active_tools,
      .workspace = prototype.workspace,
      .path_locks = prototype.path_locks,
      .resolved_path = std::nullopt,
      .output_caps = prototype.output_caps,
      .parent_turn_id = prototype.parent_turn_id,
      .scope_key = prototype.scope_key,
      .agent_key = prototype.agent_key,
      .identity = prototype.identity,
  };
}

core::Result<void> Registry::add(core::ToolDef def, Handler handler) {
  if (!handler) {
    return std::unexpected(core::Error::invalid_argument("tool handler must not be empty").with("tool", def.name));
  }
  auto prepare = [handler = std::move(handler),
                  name = def.name,
                  lockable = detail::path_lock_direction(def.required_capabilities).has_value()](
                     std::string_view input) -> core::Result<PreparedCall> {
    return PreparedCall{
        .path = lockable ? detail::prepare_lock_path(name, input) : std::nullopt,
        // Dispatch retains both the registered handler and final input bytes.
        .execute = [&handler, input](DispatchContext& ctx) { return handler(input, ctx); },
    };
  };
  return add_prepared(std::move(def), std::move(prepare));
}

core::Result<void> Registry::add_prepared(core::ToolDef def, Preparer prepare, DispatchPolicy policy) {
  if (def.name.empty()) {
    return std::unexpected(core::Error::invalid_argument("tool definition must have a non-empty name"));
  }
  if (!prepare) {
    return std::unexpected(core::Error::invalid_argument("tool preparer must not be empty").with("tool", def.name));
  }
  if (policy == DispatchPolicy::runtime && !def.required_capabilities.empty())
    return std::unexpected(core::Error::invalid_argument("runtime tools cannot declare external capabilities"));
  if (auto valid_schema = detail::validate_input_schema(def.name, def.input_schema_json); !valid_schema) {
    return std::unexpected(std::move(valid_schema).error());
  }
  auto [it, inserted] = entries_.try_emplace(def.name);
  if (!inserted) {
    return std::unexpected(core::Error{core::ErrorKind::conflict, "tool is already registered"}.with("tool", def.name));
  }
  it->second.def = std::move(def);
  it->second.prepare = std::move(prepare);
  it->second.policy = policy;
  it->second.insertion_index = next_index_++;
  return {};
}

core::Result<void> Registry::remove(std::string_view name) {
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    return std::unexpected(make_lookup_error(name));
  }
  entries_.erase(it);
  return {};
}

const core::ToolDef* Registry::find(std::string_view name) const {
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    return nullptr;
  }
  return &it->second.def;
}

std::vector<core::ToolDef> Registry::catalog() const {
  std::vector<const Entry*> ordered;
  ordered.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    ordered.push_back(&entry);
  }
  std::ranges::sort(ordered, {}, [](const Entry* entry) { return entry->insertion_index; });
  std::vector<core::ToolDef> defs;
  defs.reserve(ordered.size());
  for (const auto* entry : ordered) {
    defs.push_back(entry->def);
  }
  return defs;
}

async::Awaitable<core::Result<Output>>
Registry::dispatch(std::string_view name, std::string_view input_json, DispatchContext& ctx) const {
  ctx.resolved_path.reset();
  if (ctx.active_tools && !std::ranges::contains(*ctx.active_tools, name))
    co_return std::unexpected(
        core::Error::not_found("tool is not enabled for this turn").with("tool", std::string{name}));
  const auto it = entries_.find(name);
  if (it == entries_.end()) {
    co_return std::unexpected(make_lookup_error(name));
  }
  const auto& entry = it->second;

  const auto started_at = core::time::now_utc();
  std::optional<std::string> rewritten_input;
  std::string_view effective_input = input_json;
  hook::HookDecision hook_decision{};
  bool hook_blocked = false;
  std::string hook_reason;
  std::optional<core::Error> blocking_publish_error;

  if (ctx.bus != nullptr) {
    auto before_decision = co_await ctx.bus->publish_blocking<hook::Event::tool_before>(
        build_before_payload(name, input_json, ctx, started_at));
    if (!before_decision) {
      blocking_publish_error = std::move(before_decision).error();
    } else {
      hook_decision = std::move(*before_decision);
      switch (hook_decision.kind) {
        case hook::HookDecisionKind::proceed:
          break;
        case hook::HookDecisionKind::veto:
          hook_blocked = true;
          hook_reason = hook_decision_reason(hook_decision, "hook veto");
          break;
        case hook::HookDecisionKind::rewrite:
          hook_reason = hook_decision_reason(hook_decision, "hook rewrite");
          if (!hook_decision.rewritten_input_json.has_value()) {
            hook_blocked = true;
            hook_reason = "hook rewrite missing rewritten_input_json";
          } else {
            rewritten_input = std::move(*hook_decision.rewritten_input_json);
            effective_input = *rewritten_input;
          }
          break;
        case hook::HookDecisionKind::require_approval:
          hook_reason = hook_decision_reason(hook_decision, "hook require_approval");
          if (entry.policy == DispatchPolicy::runtime) {
            hook_blocked = true;
            hook_reason = "runtime tools do not support approval escalation";
          }
          break;
      }
    }
  }

  const bool hook_rewrote = rewritten_input.has_value();
  const auto with_hook_facts = [&](std::string metadata) {
    if (!hook_blocked && !hook_rewrote && hook_decision.trace.empty()) {
      return metadata;
    }
    return detail::with_hook_decision_metadata(
        metadata,
        hook_decision.trace,
        hook_blocked || hook_rewrote ? std::optional{input_hash_hex(input_json)} : std::nullopt,
        hook_rewrote ? std::optional{input_hash_hex(effective_input)} : std::nullopt);
  };

  core::Result<PreparedCall> prepared;
  if (!blocking_publish_error && !hook_blocked) {
    prepared = entry.prepare(effective_input);
    if (prepared && !prepared->execute) {
      prepared = std::unexpected(core::Error::internal("tool preparation returned no executor"));
    }
    if (prepared && prepared->path && entry.policy == DispatchPolicy::runtime)
      prepared = std::unexpected(core::Error::invalid_argument("runtime tools cannot request filesystem authority"));
  }

  core::Result<detail::PathLockGuard> path_lock;
  if (prepared && prepared->path && ctx.workspace != nullptr && ctx.path_locks != nullptr) {
    if (auto direction = detail::path_lock_direction(entry.def.required_capabilities)) {
      if (auto key = ctx.workspace->lock_key(prepared->path->path, *direction)) {
        const auto mode =
            *direction == LockDirection::read ? detail::PathLockMode::shared : detail::PathLockMode::exclusive;
        const auto wait_started = std::chrono::steady_clock::now();
        path_lock = co_await ctx.path_locks->table_->acquire(ctx.executor, std::move(*key), mode);
        ctx.now = core::Time{ctx.now.to_system_time_point() + std::chrono::duration_cast<core::Time::clock::duration>(
                                                                  std::chrono::steady_clock::now() - wait_started)};
      }
    }
  }

  core::Result<Output> result = std::unexpected(core::Error::internal("dispatch did not produce a result"));
  if (!path_lock) {
    result = std::unexpected(std::move(path_lock).error().with("tool", std::string{name}));
    // A cancelled lock wait has no authority or approval to release. Finish
    // only its observations before returning to the scheduler's cleanup join.
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  } else if (blocking_publish_error.has_value()) {
    result = std::unexpected(std::move(*blocking_publish_error).with("tool", std::string{name}));
  } else if (hook_blocked) {
    auto event = build_event(name, effective_input, hook_blocked_decision(hook_reason), ctx, with_hook_facts("{}"));
    event.outcome = permission::AuditOutcome::blocked_by_hook;

    if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
      result = std::unexpected(std::move(recorded).error());
    } else {
      result =
          std::unexpected(hook_decision.kind == hook::HookDecisionKind::rewrite ? hook_rewrite_error(name, hook_reason)
                                                                                : hook_veto_error(name, hook_reason));
    }
  } else if (!prepared) {
    auto event = build_event(name,
                             effective_input,
                             permission::Decision{.verdict = permission::Verdict::deny,
                                                  .reason = prepared.error().kind() == core::ErrorKind::invalid_argument
                                                                ? "invalid_tool_input"
                                                                : "tool_preparation_failed"},
                             ctx,
                             with_hook_facts("{}"));
    if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
      result = std::unexpected(std::move(recorded).error());
    } else {
      result = std::unexpected(std::move(prepared).error().with("tool", std::string{name}));
    }
  } else {
    auto path_resolution = prepared->path && ctx.workspace != nullptr
                               ? detail::resolve_tool_path(*ctx.workspace, *prepared->path)
                               : detail::PathResolutionReport{};
    ctx.resolved_path = std::move(path_resolution.path);
    auto decision = permission::Decision{.verdict = permission::Verdict::allow, .reason = "runtime_tool"};
    if (entry.policy == DispatchPolicy::permissioned) {
      decision = permission::evaluate(ctx.rules, name, effective_input, entry.def.required_capabilities, ctx.mode);
    }
    if (entry.policy == DispatchPolicy::permissioned && ctx.parent_policy) {
      auto parent = permission::evaluate(ctx.parent_policy->rules,
                                         name,
                                         effective_input,
                                         entry.def.required_capabilities,
                                         ctx.parent_policy->mode);
      decision = permission::intersect(std::move(parent), std::move(decision));
    }
    if (hook_decision.kind == hook::HookDecisionKind::require_approval) {
      decision = require_approval_decision(std::move(decision), hook_decision);
    }
    if (path_resolution.requires_approval) {
      decision = require_workspace_override_approval(std::move(decision));
    }

    auto event =
        build_event(name, effective_input, decision, ctx, with_hook_facts(std::move(path_resolution.metadata_json)));
    if (hook_rewrote && decision.verdict == permission::Verdict::allow) {
      event.outcome = permission::AuditOutcome::rewritten;
    }

    // Resolve approval before recording its durable outcome.
    auto approval = detail::ApprovalResolution{};
    const bool approval_possible = !path_resolution.error.has_value() && decision.verdict == permission::Verdict::ask &&
                                   ctx.approval_broker != nullptr;
    if (approval_possible) {
      approval = co_await detail::resolve_ask(detail::ApprovalRequest{
          .tool_name = name,
          .input = effective_input,
          .identity = ctx.identity,
          .decision = decision,
          .now = ctx.now,
          .broker = ctx.approval_broker,
          .token = ctx.approval_token,
          .token_output = ctx.approval_token_output,
          .bus = ctx.bus,
          .payload = build_permission_ask_payload(name, effective_input, ctx, decision),
      });
      if (!approval.trace.empty()) {
        event.metadata_json = detail::with_permission_ask_metadata(event.metadata_json, approval.trace);
      }
      if (approval.state == detail::ApprovalState::approved) {
        event.outcome = permission::AuditOutcome::approved;
      } else if (approval.state == detail::ApprovalState::rejected) {
        event.outcome = permission::AuditOutcome::rejected;
        if (approval.audit_reason.has_value() && !approval.audit_reason->empty()) {
          event.reason = *approval.audit_reason;
        }
      }
    }

    // The decision row is durable before the handler runs.
    const bool handler_about_to_run =
        !path_resolution.error.has_value() &&
        (decision.verdict == permission::Verdict::allow ||
         (decision.verdict == permission::Verdict::ask && approval.state == detail::ApprovalState::approved));

    if (auto recorded = co_await ctx.audit.record(std::move(event)); !recorded) {
      result = std::unexpected(std::move(recorded).error());
    } else if (path_resolution.error.has_value()) {
      result = std::unexpected(std::move(*path_resolution.error).with("tool", std::string{name}));
    } else {
      if (handler_about_to_run) {
        result = co_await prepared->execute(ctx);
      } else if (decision.verdict == permission::Verdict::deny) {
        result = std::unexpected(core::Error::permission_denied("tool denied by permission rules")
                                     .with("tool", std::string{name})
                                     .with("reason", decision.reason));
      } else if (approval.state == detail::ApprovalState::rejected) {
        result = std::unexpected(std::move(*approval.rejection).with("tool", std::string{name}));
      } else {
        result = std::unexpected(core::Error::permission_denied("tool requires approval")
                                     .with("tool", std::string{name})
                                     .with("reason", "approval_required")
                                     .with("decision_reason", decision.reason)
                                     .with("replay_max", std::to_string(decision.replay_max))
                                     .with("approval_ttl_seconds", std::to_string(decision.approval_ttl.count())));
      }
    }
  }

  if (result.has_value()) {
    [[maybe_unused]] const auto cap_report = apply_output_caps(*result, ctx.output_caps);
  }

  if (ctx.bus != nullptr) {
    co_await ctx.bus->publish_advisory(
        hook::Event::tool_after,
        build_after_payload(name, effective_input, ctx, started_at, core::time::now_utc(), result));
  }

  co_return result;
}

}  // namespace orangutan::tool
