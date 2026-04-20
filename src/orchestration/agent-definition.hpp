#pragma once

#include <string>
#include <vector>

namespace orangutan::orchestration {

    struct AgentDefinition {
        std::string key;
        std::string description;
        std::vector<std::string> tools;
        std::vector<std::string> disallowed_tools;
        std::string model;           // empty = inherit from config
        int max_turns = 0;           // 0 = no limit
        std::string prompt_addendum; // extra system prompt text
        std::string source;          // "builtin", "config", "directory"
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::AgentDefinition;
} // namespace orangutan
