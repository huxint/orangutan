// Private approval state machine used by Registry::dispatch.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/error.hpp>
#include <oran/core/time.hpp>
#include <oran/hook/payload.hpp>
#include <oran/hook/sink.hpp>
#include <oran/permission/approval.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::hook {
class Bus;
}

namespace orangutan::permission {
class ApprovalBroker;
}

namespace orangutan::tool::detail {

enum class ApprovalState {
  pending,
  approved,
  rejected,
};

struct ApprovalRequest {
  std::string_view tool_name;
  std::string_view input;
  std::string_view identity;
  const permission::Decision& decision;
  core::Time now{};
  permission::ApprovalBroker* broker{nullptr};
  const permission::ApprovalToken* token{nullptr};
  permission::ApprovalToken* token_output{nullptr};
  hook::Bus* bus{nullptr};
  hook::PermissionAskRenderedPayload payload;
};

struct ApprovalResolution {
  ApprovalState state{ApprovalState::pending};
  std::optional<core::Error> rejection{};
  std::optional<std::string> audit_reason{};
  std::vector<hook::HookDecisionTrace> trace{};
};

[[nodiscard]] async::Awaitable<ApprovalResolution> resolve_ask(ApprovalRequest request);

}  // namespace orangutan::tool::detail
