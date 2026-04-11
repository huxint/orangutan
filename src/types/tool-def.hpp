#pragma once

#include "types/content.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace orangutan {

    // Tool definition sent to the API
    struct ToolDef {
        std::string name;
        std::string description;
        nlohmann::json input_schema; // JSON Schema
    };

    // API response metadata
    struct LLMResponse {
        std::string stop_reason; // "end_turn", "tool_use", etc.
        std::vector<Content> content;
    };
} // namespace orangutan
