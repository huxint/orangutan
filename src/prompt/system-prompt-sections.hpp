#pragma once

#include <string>
#include <string_view>

namespace orangutan::prompt {

    // ── Dynamic sections (session-specific) ──

    struct EnvironmentInfo {
        std::string workspace_root;
        std::string model_name;
        std::string agent_key;
        bool is_channel_mode = false;
        bool is_sandboxed = false;
    };

    // Build the complete default system prompt from all sections
    [[nodiscard]]
    std::string build_default_system_prompt(const EnvironmentInfo &env_info);

} // namespace orangutan::prompt
