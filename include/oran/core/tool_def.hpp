// include/oran/core/tool_def.hpp — canonical tool declaration.
//
// `ToolDef` is the per-tool *declaration* every layer agrees on — the agent
// loop advertises it, the provider adapter ships it on the wire, and the
// tool registry executes against it. The runtime/execution side (dispatch, permission,
// hook plumbing) lives in `oran-tool`; this header owns the declaration shape
// so the rest of the stack can mention it without pulling in registry code.
//
// The JSON Schema payload stays an opaque `std::string` so `oran-core` stays
// nlohmann-free (rule C6); `oran-tool::Registry::add` performs registration
// sanity checks and provider adapters own vendor-specific projection.

#pragma once

#include <string>
#include <vector>

#include <oran/core/capability.hpp>

namespace orangutan::core {

struct ToolDef {
  /// Stable identifier callers reference. Required.
  std::string name;
  /// Human-readable description; surfaced to the model and to the user.
  std::string description;
  /// JSON Schema describing accepted inputs. Opaque at this layer; consumers
  /// in `oran-provider` / `oran-tool` parse and validate when they need to.
  std::string input_schema_json;
  /// Every declared capability must be authorized at dispatch. This list
  /// describes the tool's requirements; registration grants no authority.
  std::vector<Capability> required_capabilities;

  friend bool operator==(const ToolDef&, const ToolDef&) = default;

  /// Fixture/test convenience for parameter-less tools. Production callers
  /// should construct `ToolDef{...}` with a real schema directly so the
  /// schema shape is not silently lost.
  [[nodiscard]] static ToolDef with_no_input(std::string name, std::string description);
};

}  // namespace orangutan::core
