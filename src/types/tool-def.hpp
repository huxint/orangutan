#pragma once

#include "types/content.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace orangutan {

    enum class response_stop_reason : std::uint8_t {
        end_turn,
        tool_use,
        max_tokens,
        unknown,
    };

    // Tool definition sent to the API
    struct ToolDef {
        std::string name;
        std::string description;
        nlohmann::json input_schema; // JSON Schema
    };

    // API response metadata
    struct LLMResponse {
        response_stop_reason stop_reason = response_stop_reason::unknown;
        std::vector<Content> content;
    };
} // namespace orangutan
