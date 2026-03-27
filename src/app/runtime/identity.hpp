#pragma once

#include <string>
#include <vector>

namespace orangutan {

    struct RuntimeIdentity {
        std::string workspace;
        std::string runtime_key;
        std::string memory_scope;
    };

    std::string resolve_workspace_root(const std::string &workspace);

    std::string derive_cli_runtime_key(const std::string &agent_key);

    std::string derive_cli_session_scope(const std::string &agent_key);

    RuntimeIdentity derive_cli_identity(const std::string &workspace_root, const std::string &agent_key);

    std::string derive_channel_runtime_key(const std::string &jid, const std::string &agent_key);

    RuntimeIdentity derive_channel_identity(const std::string &workspace_root, const std::string &jid, const std::string &agent_key);

    RuntimeIdentity derive_child_identity(const std::string &workspace_root, const std::string &raw_caller_id, const std::string &agent_key);

    std::string append_subagent_prompt_guidance(const std::string &system_prompt, const std::vector<std::string> &allowed_child_agents, bool is_child_run);

} // namespace orangutan
