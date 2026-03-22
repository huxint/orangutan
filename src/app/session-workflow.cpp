#include "app/session-workflow.hpp"

#include <sstream>

namespace orangutan::app {

SessionMetadata make_cli_session_metadata(const std::string &model, const std::string &scope_key, const std::string &agent_key, const std::string &origin_ref) {
    return SessionMetadata{
        .model = model,
        .scope_key = scope_key,
        .agent_key = agent_key,
        .origin_kind = "cli",
        .origin_ref = origin_ref,
    };
}

std::optional<std::string> resolve_requested_session(SessionStore &store, const std::string &requested, const std::string &scope_key, const std::string &agent_key) {
    if (requested != "latest") {
        return requested;
    }

    const auto sessions = !scope_key.empty() ? store.list_sessions(scope_key) : (!agent_key.empty() ? store.list_sessions_for_agent(agent_key) : store.list_sessions());
    if (sessions.empty()) {
        return std::nullopt;
    }
    return sessions.front().id;
}

bool persist_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const SessionMetadata &metadata) {
    const auto &history = agent.history();
    if (history.empty()) {
        return false;
    }

    if (!current_session_id.empty()) {
        store.update(current_session_id, history, metadata);
    } else {
        current_session_id = store.save(history, metadata);
    }

    return true;
}

NewSessionResult start_new_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const SessionMetadata &metadata) {
    NewSessionResult result{
        .had_history = !agent.history().empty(),
        .distillation = {},
    };

    if (result.had_history) {
        result.distillation = agent.distill_session_memory();
        (void)persist_session(agent, store, current_session_id, metadata);
        result.previous_session_id = current_session_id;
    }

    agent.clear_history();
    current_session_id.clear();
    return result;
}

LoadSessionResult load_session_into_agent(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id,
                                          const std::string &scope_key, const std::string &agent_key) {
    const auto resolved_session_id = resolve_requested_session(store, requested_session_id, scope_key, agent_key);
    if (!resolved_session_id.has_value()) {
        return {.loaded = false, .status = "Error: no saved sessions available in the current scope."};
    }

    const auto &target_session_id = *resolved_session_id;
    if (!agent_key.empty() && !store.session_belongs_to_agent(target_session_id, agent_key)) {
        return {.loaded = false, .status = "Error: session does not belong to the current agent."};
    }
    if (!scope_key.empty() && !store.session_belongs_to_scope(target_session_id, scope_key)) {
        return {.loaded = false, .status = "Error: session does not belong to the current agent scope."};
    }

    try {
        auto messages = store.load(target_session_id);
        agent.set_history(std::move(messages));
        current_session_id = target_session_id;
        return {.loaded = true, .status = "Session loaded: " + target_session_id};
    } catch (const std::exception &e) {
        return {.loaded = false, .status = std::string{"Error: "} + e.what()};
    }
}

std::string describe_new_session_result(const NewSessionResult &result, bool mention_previous_session) {
    std::ostringstream out;
    out << "## Session\n";

    if (!result.had_history) {
        out << "- Started a new session.";
        return out.str();
    }

    if (result.distillation.distilled) {
        out << "- Started a new session.\n";
        out << "- Distilled `" << result.distillation.memories_stored << "` long-term memories.";
    } else if (!result.distillation.status.empty()) {
        out << "- Started a new session.\n";
        out << "- " << result.distillation.status;
    } else {
        out << "- Started a new session.";
    }

    if (mention_previous_session) {
        out << "\n- Previous session remains available to resume later.";
    }
    return out.str();
}

} // namespace orangutan::app
