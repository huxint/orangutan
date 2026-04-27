#pragma once

#include "agent/agent-loop.hpp"
#include "providers/provider.hpp"
#include "storage/session-store.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace orangutan::automation {
    class AutomationRuntime;
}

namespace orangutan::memory {
    class RuntimeMemory;
}

namespace orangutan::cli {

    using SessionDistillationDispatcher = std::function<void(std::vector<Message>)>;

    struct NewSessionResult {
        bool had_history = false;
        std::string previous_session_id;
        std::string new_session_id;
        AgentLoop::SessionMemoryDistillationResult distillation;
    };

    struct LoadSessionResult {
        bool loaded = false;
        std::string status;
    };

    struct SessionExportResult {
        bool exported = false;
        std::string path;
        std::string status;
    };

    [[nodiscard]]
    std::optional<std::string> resolve_requested_session(SessionStore &store, const std::string &requested, const std::string &scope_key, const std::string &agent_key = {});

    [[nodiscard]]
    SessionMetadata make_cli_session_metadata(const std::string &model, const std::string &scope_key, const std::string &agent_key, const std::string &origin_ref = "cli:local");

    [[nodiscard]]
    bool persist_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const SessionMetadata &metadata);

    [[nodiscard]]
    SessionDistillationDispatcher make_background_session_distillation_dispatcher(automation::AutomationRuntime *automation_runtime, const ProviderSystem &provider,
                                                                                  ProviderRoute route, const memory::RuntimeMemory *memory);

    [[nodiscard]]
    NewSessionResult start_new_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const SessionMetadata &metadata,
                                       const SessionDistillationDispatcher &dispatch_distillation = {});

    [[nodiscard]]
    LoadSessionResult load_session_into_agent(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id,
                                              const std::string &scope_key = {}, const std::string &agent_key = {});

    [[nodiscard]]
    std::string describe_new_session_result(const NewSessionResult &result, bool mention_previous_session);
    [[nodiscard]]
    SessionExportResult export_session_markdown(const std::vector<Message> &history, const std::string &session_id, const std::string &workspace_root);
    [[nodiscard]]
    std::string describe_export_result(const SessionExportResult &result);

} // namespace orangutan::cli
