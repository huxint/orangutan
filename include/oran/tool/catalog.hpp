#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <oran/core/result.hpp>
#include <oran/core/tool_def.hpp>

namespace orangutan::tool {

/// Select owned definitions sorted by name. Absent names select the entire
/// catalogue; an empty list selects none. Unknown or ambiguous names fail.
/// Selection controls model exposure and availability, never external authority.
[[nodiscard]] core::Result<std::vector<core::ToolDef>>
select_tools(std::span<const core::ToolDef> catalog, std::optional<std::span<const std::string>> names = std::nullopt);

}  // namespace orangutan::tool
