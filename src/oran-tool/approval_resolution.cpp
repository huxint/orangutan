// src/oran-tool/approval_resolution.cpp — ask/broker approval state machine.

#include "_impl/approval_resolution.hpp"

#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/enum_names.hpp>
#include <oran/hook/bus.hpp>
#include <oran/hook/event.hpp>
#include <oran/permission/approval_broker.hpp>

namespace orangutan::tool::detail {
namespace {

[[nodiscard]] std::string_view decision_reason(const hook::HookDecision& decision, std::string_view fallback) {
  return decision.reason.empty() ? fallback : std::string_view{decision.reason};
}

[[nodiscard]] std::string_view broker_reason(const core::Error& error) noexcept {
  const auto entries = error.context();
  const auto it = std::ranges::find_if(entries, [](const auto& entry) { return entry.first == "reason"; });
  return it != entries.end() ? std::string_view{it->second} : std::string_view{};
}

[[nodiscard]] std::string_view operator_reason(const hook::HookDecision& decision) {
  if (!decision.reason.empty()) {
    return decision.reason;
  }
  auto reversed_trace = decision.trace | std::views::reverse;
  const auto it =
      std::ranges::find_if(reversed_trace, [](const hook::HookDecisionTrace& trace) { return !trace.reason.empty(); });
  return it == reversed_trace.end() ? std::string_view{} : std::string_view{it->reason};
}

[[nodiscard]] core::Error operator_denied_error(std::string reason) {
  if (reason.empty()) {
    reason = "permission_ask_veto";
  }
  return core::Error::permission_denied("operator denied approval")
      .with("reason", "operator_denied")
      .with("hook_reason", std::move(reason));
}

[[nodiscard]] core::Error unsupported_decision_error(const hook::HookDecision& decision) {
  return core::Error::permission_denied("permission approval prompt returned an unsupported decision")
      .with("reason", "operator_denied")
      .with("decision_kind", std::string{core::enum_name(decision.kind)})
      .with("hook_reason", std::string{decision_reason(decision, "unsupported_permission_ask_decision")});
}

[[nodiscard]] core::Error missing_operator_reason_error() {
  return core::Error::permission_denied("permission approval prompt returned proceed without operator identity")
      .with("reason", "operator_denied")
      .with("decision_kind", std::string{core::enum_name(hook::HookDecisionKind::proceed)})
      .with("hook_reason", "permission_ask_missing_operator_reason");
}

[[nodiscard]] ApprovalResolution rejected(core::Error error, std::string audit_reason) {
  return ApprovalResolution{
      .state = ApprovalState::rejected,
      .rejection = std::move(error),
      .audit_reason = std::move(audit_reason),
  };
}

[[nodiscard]] ApprovalResolution check_token(const ApprovalRequest& request, const permission::ApprovalToken& token) {
  auto checked = request.broker->check(token, request.tool_name, request.input, request.identity, request.now);
  if (checked) {
    return ApprovalResolution{.state = ApprovalState::approved};
  }
  auto reason = std::string{broker_reason(checked.error())};
  auto resolution = rejected(std::move(checked).error(), reason);
  return resolution;
}

}  // namespace

async::Awaitable<ApprovalResolution> resolve_ask(ApprovalRequest request) {
  if (request.broker == nullptr) {
    co_return ApprovalResolution{};
  }
  if (request.token != nullptr) {
    co_return check_token(request, *request.token);
  }
  if (request.bus == nullptr) {
    co_return ApprovalResolution{};
  }

  auto ask_decision =
      co_await request.bus->publish_blocking<hook::Event::permission_ask_rendered>(std::move(request.payload));
  if (!ask_decision) {
    co_return rejected(std::move(ask_decision).error(), "operator_denied");
  }
  if (ask_decision->trace.empty()) {
    co_return ApprovalResolution{};
  }

  auto trace = ask_decision->trace;
  switch (ask_decision->kind) {
    case hook::HookDecisionKind::proceed: {
      if (operator_reason(*ask_decision).empty()) {
        auto resolution = rejected(missing_operator_reason_error(), "operator_denied");
        resolution.trace = std::move(trace);
        co_return resolution;
      }
      auto issued = request.broker->approve(
          permission::ApprovalGrant{
              .tool_name = request.tool_name,
              .input = request.input,
              .identity = request.identity,
              .ttl = request.decision.approval_ttl,
              .replay_max = request.decision.replay_max,
          },
          request.now);
      const auto* token_to_check = &issued;
      if (request.token_output != nullptr) {
        *request.token_output = std::move(issued);
        token_to_check = request.token_output;
      }
      auto resolution = check_token(request, *token_to_check);
      resolution.trace = std::move(trace);
      co_return resolution;
    }
    case hook::HookDecisionKind::veto: {
      auto resolution = rejected(operator_denied_error(ask_decision->reason), "operator_denied");
      resolution.trace = std::move(trace);
      co_return resolution;
    }
    case hook::HookDecisionKind::rewrite:
    case hook::HookDecisionKind::require_approval: {
      auto resolution = rejected(unsupported_decision_error(*ask_decision), "operator_denied");
      resolution.trace = std::move(trace);
      co_return resolution;
    }
  }
  co_return rejected(core::Error::internal("unreachable approval decision"), "operator_denied");
}

}  // namespace orangutan::tool::detail
