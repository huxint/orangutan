#include "bootstrap/cli-runtime.hpp"

#include "agent/agent-loop.hpp"
#include "storage/session-store.hpp"

#include <cstdio>
#include <iostream>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace {

    bool choose_resume_session_id(const std::vector<orangutan::storage::SessionInfo> &sessions, std::string &resume_session) {
        if (sessions.empty()) {
            spdlog::fmt_lib::println(stderr, "Error: no saved sessions to resume.");
            spdlog::fmt_lib::println(stderr, "Start a conversation first - sessions are auto-saved on exit.");
            return false;
        }

        if (resume_session == "latest" || sessions.size() == 1) {
            resume_session = sessions[0].id;
            return true;
        }

        if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) {
            resume_session = sessions[0].id;
            return true;
        }

        spdlog::fmt_lib::println("Available sessions:");
        for (std::size_t index = 0; index < sessions.size(); ++index) {
            const auto &session = sessions[index];
            spdlog::fmt_lib::println("  [{}] {}  {}  {}  ({} messages)", index + 1, session.id, session.created_at, session.model, session.message_count);
        }
        spdlog::fmt_lib::print("\nEnter number (or press Enter for latest): ");
        std::fflush(stdout);

        std::string choice;
        std::getline(std::cin, choice);
        if (choice.empty()) {
            resume_session = sessions[0].id;
            return true;
        }

        try {
            const auto idx = std::stoul(choice) - 1;
            if (idx >= sessions.size()) {
                spdlog::fmt_lib::println(stderr, "Invalid selection.");
                return false;
            }
            resume_session = sessions[idx].id;
            return true;
        } catch (const std::exception &) {
            spdlog::fmt_lib::println(stderr, "Invalid selection.");
            return false;
        }
    }

} // namespace

namespace orangutan::bootstrap {

    bool restore_requested_session(const CliOptions &options, storage::SessionStore &session_store, const AgentRuntimeConfig &runtime_cfg, agent::AgentLoop &agent,
                                   std::string &resume_session, std::string &current_session_id) {
        if (!options.resume_requested) {
            return true;
        }

        if (resume_session.empty() || resume_session == "latest") {
            auto sessions = session_store.list_sessions(runtime_cfg.cli_memory_scope);
            if (!choose_resume_session_id(sessions, resume_session)) {
                return false;
            }
        }

        try {
            if (!runtime_cfg.cli_memory_scope.empty() && !session_store.session_belongs_to_scope(resume_session, runtime_cfg.cli_memory_scope)) {
                spdlog::fmt_lib::println(stderr, "Error: session does not belong to agent '{}'.", options.cli_agent_key);
                return false;
            }
            auto messages = session_store.load(resume_session);
            agent.set_history(std::move(messages));
            current_session_id = resume_session;
            if (!options.event_stream) {
                spdlog::fmt_lib::println("Resumed session: {}", resume_session);
            }
            return true;
        } catch (const std::exception &) {
            spdlog::fmt_lib::println(stderr, "Error: session not found: {}", resume_session);
            auto sessions = session_store.list_sessions(runtime_cfg.cli_memory_scope);
            if (sessions.empty()) {
                spdlog::fmt_lib::println(stderr, "No saved sessions available.");
            } else {
                spdlog::fmt_lib::println(stderr, "Available sessions:");
                for (const auto &session : sessions) {
                    spdlog::fmt_lib::println(stderr, "  {}  {}  {}  ({} messages)", session.id, session.created_at, session.model, session.message_count);
                }
            }
            return false;
        }
    }

    std::string merge_stdin_message(std::string message) {
        auto stdin_content = read_stdin_if_piped();
        if (stdin_content.empty()) {
            return message;
        }

        if (message.empty()) {
            return stdin_content;
        }
        return message + "\n\n" + stdin_content;
    }

    bool validate_runtime_mode_options(const CliOptions &options, bool has_current_session) {
        if (options.event_stream && options.message.empty() && !options.dump_session) {
            spdlog::fmt_lib::println(stderr, "Error: --event-stream requires --message or piped stdin.");
            return false;
        }
        if (!options.dump_session) {
            return true;
        }
        if (!options.event_stream) {
            spdlog::fmt_lib::println(stderr, "Error: --dump-session requires --event-stream.");
            return false;
        }
        if (!has_current_session) {
            spdlog::fmt_lib::println(stderr, "Error: --dump-session requires --resume.");
            return false;
        }
        return true;
    }

} // namespace orangutan::bootstrap
