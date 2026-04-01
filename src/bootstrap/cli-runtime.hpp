#pragma once

#include "bootstrap/cli-options.hpp"
#include "bootstrap/config-builder.hpp"

#include <string>

namespace orangutan::agent {
    class AgentLoop;
}

namespace orangutan::storage {
    class SessionStore;
}

namespace orangutan::bootstrap {

    [[nodiscard]]
    bool restore_requested_session(const CliOptions &options, storage::SessionStore &session_store, const AgentRuntimeConfig &runtime_cfg, agent::AgentLoop &agent,
                                   std::string &resume_session, std::string &current_session_id);

    [[nodiscard]]
    std::string merge_stdin_message(std::string message);

    [[nodiscard]]
    bool validate_runtime_mode_options(const CliOptions &options, bool has_current_session);

} // namespace orangutan::bootstrap
