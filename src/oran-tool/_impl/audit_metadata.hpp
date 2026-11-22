// Internal audit metadata helpers for tool dispatch.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <oran/hook/decision.hpp>

namespace orangutan::tool::detail {

[[nodiscard]] std::string with_hook_decision_metadata(std::string_view metadata_json,
                                                      std::span<const hook::HookDecisionTrace> trace,
                                                      std::optional<std::string> original_input_hash,
                                                      std::optional<std::string> rewritten_input_hash);

[[nodiscard]] std::string with_permission_ask_metadata(std::string_view metadata_json,
                                                       std::span<const hook::HookDecisionTrace> trace);

}  // namespace orangutan::tool::detail
