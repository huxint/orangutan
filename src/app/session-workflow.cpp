#include "app/session-workflow.hpp"

namespace orangutan::app {

std::optional<std::string> resolve_requested_session(SessionStore &store, const std::string &requested, const std::string &scope_key) {
    if (requested != "latest") {
        return requested;
    }

    const auto sessions = store.list_sessions(scope_key);
    if (sessions.empty()) {
        return std::nullopt;
    }
    return sessions.front().id;
}

bool persist_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key) {
    const auto &history = agent.history();
    if (history.empty()) {
        return false;
    }

    if (!current_session_id.empty()) {
        store.update(current_session_id, history);
    } else {
        current_session_id = store.save(history, model, scope_key);
    }

    return true;
}

NewSessionResult start_new_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key) {
    NewSessionResult result{
        .had_history = !agent.history().empty(),
        .distillation = {},
    };

    if (result.had_history) {
        result.distillation = agent.distill_session_memory();
        (void)persist_session(agent, store, model, current_session_id, scope_key);
        result.previous_session_id = current_session_id;
    }

    agent.clear_history();
    current_session_id.clear();
    return result;
}

LoadSessionResult load_session_into_agent(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id,
                                          const std::string &scope_key) {
    const auto resolved_session_id = resolve_requested_session(store, requested_session_id, scope_key);
    if (!resolved_session_id.has_value()) {
        return {.loaded = false, .status = "Error: no saved sessions available in the current scope."};
    }

    const auto &target_session_id = *resolved_session_id;
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
    if (!result.had_history) {
        return "Started a new session.";
    }

    std::string message;
    if (result.distillation.distilled) {
        message = "Started a new session. Distilled " + std::to_string(result.distillation.memories_stored) + " long-term memories.";
    } else {
        message = "Started a new session. " + result.distillation.status;
    }

    if (mention_previous_session) {
        message += " Previous session remains available to resume later.";
    }
    return message;
}

} // namespace orangutan::app
