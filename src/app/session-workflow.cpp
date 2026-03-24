#include "app/session-workflow.hpp"
#include "infra/files/file.hpp"

#include <filesystem>
#include <format>
#include <print>

namespace orangutan::app {

namespace {

std::string message_heading(const Message &message, size_t index) {
    if (message.role == Role::User) {
        return "User " + std::to_string(index + 1);
    }
    if (message.role == Role::Assistant) {
        return "Assistant " + std::to_string(index + 1);
    }
    return std::string(role_to_string(message.role)) + ' ' + std::to_string(index + 1);
}

void append_message_markdown(std::FILE *file, const Message &message, size_t index) {
    std::println(file, "## {}\n", message_heading(message, index));

    if (message.content.empty()) {
        std::println(file, "_Empty message._\n");
        return;
    }

    for (const auto &block : message.content) {
        if (const auto *text = std::get_if<TextBlock>(&block)) {
            std::println(file, "{}\n", text->text);
            continue;
        }
        if (const auto *tool = std::get_if<ToolUseBlock>(&block)) {
            std::println(file, "### Tool Use: `{}`\n", tool->name);
            std::println(file, "```json\n{}\n```\n", tool->input.dump(2));
            continue;
        }
        if (const auto *result = std::get_if<ToolResultBlock>(&block)) {
            std::println(file, "{}\n", result->is_error ? "### Tool Result (error)" : "### Tool Result");
            std::println(file, "```text\n{}\n```\n", result->content);
        }
    }
}

} // namespace

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

    const auto sessions = [&]() {
        if (!scope_key.empty()) {
            return store.list_sessions(scope_key);
        }
        if (!agent_key.empty()) {
            return store.list_sessions_for_agent(agent_key);
        }
        return store.list_sessions();
    }();
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
        static_cast<void>(persist_session(agent, store, current_session_id, metadata));
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
    static_cast<void>(mention_previous_session);
    static_cast<void>(result);
    return "## Session\n- ✨ Started a new session.";
}

SessionExportResult export_session_markdown(const std::vector<Message> &history, const std::string &session_id, const std::string &workspace_root) {
    if (workspace_root.empty()) {
        return {.status = "Workspace root is not available."};
    }
    if (session_id.empty()) {
        return {.status = "No active session to export."};
    }
    if (history.empty()) {
        return {.status = "No session history to export."};
    }

    const auto export_dir = std::filesystem::path(workspace_root) / ".exports";
    const auto export_path = export_dir / (session_id + ".md");

    std::error_code ec;
    std::filesystem::create_directories(export_dir, ec);
    if (ec) {
        return {.status = "Failed to create export directory: " + ec.message()};
    }

    std::optional<fileio::File> file;
    try {
        file.emplace(export_path, "w");
    } catch (const std::runtime_error &) {
        return {.status = "Failed to open export file for writing."};
    }

    try {
        std::println(file->get(), "# Session Export\n");
        std::println(file->get(), "- Session: `{}`", session_id);
        std::println(file->get(), "- Messages: `{}`\n", history.size());

        for (size_t index = 0; index < history.size(); ++index) {
            append_message_markdown(file->get(), history[index], index);
        }

        file->close();
    } catch (const std::exception &) {
        return {.status = "Failed to write export file."};
    }

    return {.exported = true, .path = export_path.string(), .status = "Exported session to " + export_path.string()};
}

std::string describe_export_result(const SessionExportResult &result) {
    if (!result.exported) {
        return "## Export\n- " + result.status;
    }

    return "## Export\n- Saved current session to `" + result.path + '`';
}

} // namespace orangutan::app
