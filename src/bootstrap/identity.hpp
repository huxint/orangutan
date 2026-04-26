#pragma once

#include "orchestration/types.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::bootstrap {

    struct RuntimeIdentity {
        std::string workspace;
        std::string runtime_key;
        std::string memory_scope;
    };

    std::string resolve_workspace_root(std::string_view workspace);
    std::filesystem::path workspace_state_root(std::string_view workspace_root);
    std::filesystem::path workspace_skills_root(std::string_view workspace_root);
    std::filesystem::path workspace_hooks_root(std::string_view workspace_root);
    std::filesystem::path workspace_memory_root(std::string_view workspace_root);
    std::filesystem::path workspace_identity_root(std::string_view workspace_root, std::string_view runtime_key);
    std::filesystem::path workspace_exports_root(std::string_view workspace_root);
    std::filesystem::path workspace_session_store_path(std::string_view workspace_root);
    std::filesystem::path workspace_memory_store_path(std::string_view workspace_root);
    std::filesystem::path workspace_automation_store_path(std::string_view workspace_root);

    std::string derive_cli_runtime_key(std::string_view agent_key);

    std::string derive_cli_session_scope(std::string_view agent_key);

    RuntimeIdentity derive_cli_identity(const std::string &workspace_root, std::string_view agent_key);

    std::string derive_channel_runtime_key(std::string_view jid, std::string_view agent_key);

    RuntimeIdentity derive_channel_identity(const std::string &workspace_root, std::string_view jid, std::string_view agent_key);

    RuntimeIdentity derive_child_identity(const std::string &workspace_root, std::string_view raw_caller_id, std::string_view agent_key);

    std::string append_agent_prompt_guidance(const std::string &system_prompt, orchestration::agent_role role);

} // namespace orangutan::bootstrap
