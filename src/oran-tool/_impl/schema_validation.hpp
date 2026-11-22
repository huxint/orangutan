// Internal schema validation helpers for `oran-tool`.

#pragma once

#include <string_view>

#include <oran/core/result.hpp>

namespace orangutan::tool::detail {

[[nodiscard]] core::Result<void> validate_input_schema(std::string_view tool, std::string_view schema_json);

}  // namespace orangutan::tool::detail
