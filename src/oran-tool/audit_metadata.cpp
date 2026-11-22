// src/oran-tool/audit_metadata.cpp — structured audit metadata helpers.

#include "_impl/audit_metadata.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/hook/decision.hpp>

namespace orangutan::tool::detail {

namespace {

// Metadata here is always produced by dispatch itself, so a non-object is a
// bug upstream; start from an empty object rather than throwing mid-dispatch.
[[nodiscard]] nlohmann::json parse_metadata_object(std::string_view metadata_json) {
  auto parsed = nlohmann::json::parse(metadata_json, nullptr, false);
  return parsed.is_object() ? parsed : nlohmann::json::object();
}

[[nodiscard]] nlohmann::json hook_decision_trace_to_json(std::span<const hook::HookDecisionTrace> trace) {
  auto rows = nlohmann::json::array();
  for (const auto& decision : trace) {
    auto row = nlohmann::json::object();
    row["sink_id"] = decision.sink_id;
    row["kind"] = std::string{core::enum_name(decision.kind)};
    row["reason"] = decision.reason;
    if (decision.elapsed.has_value()) {
      row["elapsed_ms"] = decision.elapsed->count();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

std::string with_hook_decision_metadata(std::string_view metadata_json,
                                        std::span<const hook::HookDecisionTrace> trace,
                                        std::optional<std::string> original_input_hash,
                                        std::optional<std::string> rewritten_input_hash) {
  auto metadata = parse_metadata_object(metadata_json);
  metadata["hook_decisions"] = hook_decision_trace_to_json(trace);
  if (original_input_hash.has_value()) {
    metadata["original_input_hash"] = std::move(*original_input_hash);
  }
  if (rewritten_input_hash.has_value()) {
    metadata["rewritten_input_hash"] = std::move(*rewritten_input_hash);
  }
  return metadata.dump();
}

std::string with_permission_ask_metadata(std::string_view metadata_json,
                                         std::span<const hook::HookDecisionTrace> trace) {
  auto metadata = parse_metadata_object(metadata_json);
  metadata["permission_ask_decisions"] = hook_decision_trace_to_json(trace);
  return metadata.dump();
}

}  // namespace orangutan::tool::detail
