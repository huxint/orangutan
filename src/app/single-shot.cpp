#include "app/single-shot.hpp"

#include "app/history-events.hpp"

#include <ostream>

namespace orangutan::app {

namespace {

json done_event(const std::string &session_id) {
    return {
        {"type", "done"},
        {"session_id", session_id},
    };
}

void emit_single_message_error(bool event_stream, const JsonEmitter &emit, std::ostream &error_stream, const std::string &message) {
    if (event_stream) {
        emit({
            {"type", "error"},
            {"message", message},
        });
        return;
    }

    error_stream << "Error: " << message << '\n';
}

auto make_stream_event_emitter(const JsonEmitter &emit) {
    return [&emit](const std::string &event_type, const json &data) {
        if (event_type == "text_delta") {
            emit({
                {"type", "assistant_delta"},
                {"text", data.value("text", "")},
            });
            return;
        }

        if (event_type == "tool_call_start") {
            emit({
                {"type", "tool_call_started"},
                {"id", data.value("id", "")},
                {"name", data.value("name", "")},
                {"input", data.value("input", json::object())},
            });
        }
    };
}

auto make_tool_event_emitter(const JsonEmitter &emit) {
    return [&emit](const std::string &event_type, const ToolUseBlock &call, const ToolResultBlock *result) {
        if (event_type == "tool_started") {
            emit({
                {"type", "tool_started"},
                {"id", call.id},
                {"name", call.name},
                {"input", call.input},
            });
            return;
        }

        if (event_type == "tool_finished" && result != nullptr) {
            emit({
                {"type", "tool_finished"},
                {"id", call.id},
                {"name", call.name},
                {"input", call.input},
                {"output", result->content},
                {"is_error", result->is_error},
                {"details", build_edit_details(call)},
            });
        }
    };
}

void run_single_message_agent(AgentLoop &agent, const std::string &message, bool event_stream, const std::string &current_session_id, const JsonEmitter &emit) {
    if (!event_stream) {
        (void)agent.run(message);
        return;
    }

    if (!current_session_id.empty()) {
        emit(make_session_event_json("session_resumed", current_session_id, "resumed"));
    }

    const auto stream_event = make_stream_event_emitter(emit);
    const auto tool_event = make_tool_event_emitter(emit);
    (void)agent.run(message, stream_event, tool_event);
}

void maybe_persist_single_message_session(AgentLoop &agent, SessionStore &session_store, const Config &cfg, std::string &current_session_id, const std::string &model,
                                          const std::string &scope_key, bool event_stream, const JsonEmitter &emit, std::ostream &error_stream) {
    if (!cfg.auto_save || agent.history().empty()) {
        return;
    }

    const bool has_existing_session = !current_session_id.empty();
    if (has_existing_session) {
        session_store.update(current_session_id, agent.history());
    } else {
        current_session_id = session_store.save(agent.history(), model, scope_key);
    }

    if (event_stream) {
        emit(make_session_event_json("session_saved", current_session_id, has_existing_session ? "updated" : "auto_saved"));
        return;
    }

    error_stream << (has_existing_session ? "Session updated: " : "Auto-saved session: ") << current_session_id << " (use -r " << current_session_id << " to resume)\n";
}

} // namespace

void emit_session_history_dump(const std::vector<Message> &history, const std::string &current_session_id, const JsonEmitter &emit) {
    emit(make_session_event_json("session_resumed", current_session_id, "resumed"));
    for (const auto &event : build_session_history_events(history)) {
        emit(event);
    }
    emit(done_event(current_session_id));
}

int run_single_message(AgentLoop &agent, SessionStore &session_store, const Config &cfg, const std::string &message, bool event_stream, std::string &current_session_id,
                       const std::string &model, const std::string &scope_key, const JsonEmitter &emit, std::ostream &error_stream) {
    try {
        run_single_message_agent(agent, message, event_stream, current_session_id, emit);
    } catch (const std::exception &e) {
        emit_single_message_error(event_stream, emit, error_stream, e.what());
        return 1;
    }

    maybe_persist_single_message_session(agent, session_store, cfg, current_session_id, model, scope_key, event_stream, emit, error_stream);

    if (event_stream) {
        emit(done_event(current_session_id));
    }
    return 0;
}

} // namespace orangutan::app
