// Internal helpers for per-sink hook input redaction.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace orangutan::tool::detail {

[[nodiscard]] std::optional<std::string> redacted_hook_input_json(std::string_view tool_name,
                                                                  std::string_view input_json);

}  // namespace orangutan::tool::detail
