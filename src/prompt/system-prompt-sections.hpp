#pragma once

#include <string>
#include <string_view>

namespace orangutan::prompt {

    // ── Static sections (stable across sessions) ──

    // Role and identity framing
    [[nodiscard]]
    std::string identity_section();

    // System behavior: output format, tool permissions, hooks, compression
    [[nodiscard]]
    std::string system_behavior_section();

    // Task execution guidelines: coding, security, no over-engineering
    [[nodiscard]]
    std::string task_guidelines_section();

    // Safety and reversibility: destructive actions, blast radius
    [[nodiscard]]
    std::string safety_actions_section();

    // Tool usage guidance: prefer dedicated tools over shell, parallel calls
    [[nodiscard]]
    std::string tool_usage_section();

    // Tone, style, formatting
    [[nodiscard]]
    std::string tone_style_section();

    // Output efficiency: conciseness, lead with action
    [[nodiscard]]
    std::string output_efficiency_section();

    // Cyber risk / security boundary
    [[nodiscard]]
    std::string_view security_instruction();

    // ── Dynamic sections (session-specific) ──

    struct EnvironmentInfo {
        std::string workspace_root;
        std::string model_name;
        std::string agent_key;
        bool is_channel_mode = false;
    };

    // Environment: cwd, git, platform, OS, model info
    [[nodiscard]]
    std::string environment_section(const EnvironmentInfo &info);

    // ── Workspace agent files ──

    // Read optional workspace file (.orangutan/agent/{identity,style,memory}.md).
    // Returns empty string if the file does not exist.
    [[nodiscard]]
    std::string read_workspace_agent_file(const std::string &workspace, const std::string &filename);

    // ── Full assembly ──

    // Build the complete default system prompt from all sections
    [[nodiscard]]
    std::string build_default_system_prompt(const EnvironmentInfo &env_info);

} // namespace orangutan::prompt
