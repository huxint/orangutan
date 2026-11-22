// src/oran-core/tool_def.cpp — `ToolDef` helpers.

#include <oran/core/tool_def.hpp>

#include <string>
#include <utility>

namespace orangutan::core {

ToolDef ToolDef::with_no_input(std::string name, std::string description) {
  return ToolDef{
      .name = std::move(name),
      .description = std::move(description),
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
      .required_capabilities = {},
  };
}

}  // namespace orangutan::core
