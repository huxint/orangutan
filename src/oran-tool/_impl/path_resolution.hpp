// Internal workspace pre-resolution helpers for `oran-tool`.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::tool::detail {

struct PathResolutionReport {
  std::optional<ResolvedToolPath> path{};
  std::string metadata_json{"{}"};
  std::optional<core::Error> error{};
  bool requires_approval{false};
};

[[nodiscard]] std::optional<LockDirection> path_lock_direction(std::span<const core::Capability> capabilities);

/// Compatibility admission for ordinary handlers; never selects built-in authority.
[[nodiscard]] std::optional<PathRequest> prepare_lock_path(std::string_view tool_name, std::string_view input_json);

/// Resolve declared authority after admission. Lock-only requests acquire none.
[[nodiscard]] PathResolutionReport resolve_tool_path(const Workspace& workspace, const PathRequest& request);

}  // namespace orangutan::tool::detail
