#include "app/repl.hpp"

#include "app/cli-ui.hpp"
#include "app/session-workflow.hpp"
#include "features/hooks/hook-manager.hpp"
#include "infra/storage/session-store.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>

#include <readline/history.h>
#include <readline/readline.h>

namespace orangutan::app {

namespace {

using ReadlinePtr = std::unique_ptr<char, decltype(&std::free)>;

std::string read_multiline() {
    std::cout << "Multi-line mode. Enter an empty line to finish.\n";
    std::string result;

    while (true) {
        ReadlinePtr line(readline("... "), &std::free);
        if (line == nullptr) {
            break;
        }

        std::string line_str(line.get());
        if (line_str.empty()) {
            break;
        }

        if (!result.empty()) {
            result += '\n';
        }
        result += line_str;
    }

    return result;
}

std::optional<std::string> read_input_line() {
    ReadlinePtr input(readline("you> "), &std::free);
    if (input == nullptr) {
        return std::nullopt;
    }

    std::string line(input.get());
    if (line.empty()) {
        return line;
    }

    add_history(line.c_str());
    while (!line.empty() && line.back() == '\\') {
        line.pop_back();
        line += '\n';

        ReadlinePtr continuation(readline("... "), &std::free);
        if (continuation == nullptr) {
            break;
        }
        line += continuation.get();
    }

    return line;
}

void save_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key,
                  HookManager *hook_manager) {
    const auto updating_existing = !current_session_id.empty();
    if (!persist_session(agent, store, model, current_session_id, scope_key)) {
        std::cout << "Nothing to save (empty history).\n\n";
        return;
    }

    if (!updating_existing) {
        dispatch_session_start(hook_manager, current_session_id, agent.history().size());
    }

    if (updating_existing) {
        std::cout << "Session updated: " << current_session_id << " (use -r " << current_session_id << " to resume)\n\n";
        return;
    }
    std::cout << "Session saved: " << current_session_id << " (use -r " << current_session_id << " to resume)\n\n";
}

void load_session(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id, const std::string &scope_key,
                  HookManager *hook_manager) {
    const auto previous_session_id = current_session_id;
    const auto previous_message_count = agent.history().size();
    const auto result = load_session_into_agent(requested_session_id, agent, store, current_session_id, scope_key);
    if (result.loaded && current_session_id != previous_session_id) {
        dispatch_session_end(hook_manager, previous_session_id, previous_message_count);
        dispatch_session_start(hook_manager, current_session_id, agent.history().size());
    }
    std::cout << result.status << "\n\n";
}

void compress_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id) {
    const auto result = agent.compress_history();
    if (!result.compacted) {
        std::cout << result.status << "\n\n";
        return;
    }

    if (!current_session_id.empty()) {
        store.update(current_session_id, agent.history());
    }

    std::cout << "Compressed history: " << result.messages_before << " -> " << result.messages_after << " messages.\n\n";
}

bool handle_slash_command(const std::string &line, AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, bool &quit, const Config &cfg,
                          const std::string &agent_key, const std::string &scope_key, const SkillLoader *skill_loader, const ToolRegistry *tool_registry,
                          HookManager *hook_manager) {
    if (line == "/quit" || line == "/exit") {
        quit = true;
        return true;
    }
    if (line == "/help") {
        std::cout << repl_help_text();
        return true;
    }
    if (line == "/new") {
        const auto previous_message_count = agent.history().size();
        const auto result = start_new_session(agent, store, model, current_session_id, scope_key);
        dispatch_session_end(hook_manager, result.previous_session_id, previous_message_count);
        std::cout << describe_new_session_result(result, false) << "\n\n";
        return true;
    }
    if (line == "/compress") {
        compress_session(agent, store, current_session_id);
        return true;
    }
    if (line == "/clear") {
        agent.clear_history();
        std::cout << "History cleared.\n\n";
        return true;
    }
    if (line == "/history") {
        std::cout << render_history_summary(agent);
        return true;
    }
    if (line == "/session") {
        if (current_session_id.empty()) {
            std::cout << "No active session.\n\n";
        } else {
            std::cout << "Current session: " << current_session_id << "\n\n";
        }
        return true;
    }
    if (line == "/save") {
        save_session(agent, store, model, current_session_id, scope_key, hook_manager);
        return true;
    }
    if (line == "/sessions") {
        std::cout << render_saved_sessions(store, scope_key);
        return true;
    }
    if (line == "/agent") {
        std::cout << "Current agent: " << agent_key << "\n\n";
        return true;
    }
    if (line == "/agents") {
        std::cout << format_agent_list(cfg, agent_key) << "\n\n";
        return true;
    }
    if (line.starts_with("/load ") || line.starts_with("/resume ")) {
        const auto session_id = line.starts_with("/resume ") ? line.substr(8) : line.substr(6);
        load_session(session_id, agent, store, current_session_id, scope_key, hook_manager);
        return true;
    }
    if (line == "/skills") {
        if (skill_loader == nullptr || skill_loader->active_skills().empty()) {
            std::cout << "No skills loaded.\n\n";
        } else {
            std::cout << "Loaded skills:\n";
            for (const auto &skill : skill_loader->active_skills()) {
                std::cout << "  " << skill.name << " — " << skill.description << "\n";
                std::cout << "    source: " << skill.source_path << "\n";
                if (!skill.tools.empty()) {
                    std::cout << "    tools: ";
                    for (size_t i = 0; i < skill.tools.size(); ++i) {
                        if (i > 0) {
                            std::cout << ", ";
                        }
                        std::cout << skill.tools[i];
                    }
                    std::cout << "\n";
                }
            }
            std::cout << "\n";
        }
        return true;
    }
    if (line == "/tools") {
        if (tool_registry == nullptr) {
            std::cout << "No tool registry available.\n\n";
        } else {
            const auto defs = tool_registry->definitions();
            if (defs.empty()) {
                std::cout << "No tools registered.\n\n";
            } else {
                std::cout << "Registered tools (" << defs.size() << "):\n";
                for (const auto &def : defs) {
                    std::cout << "  " << def.name << " — " << def.description << "\n";
                }
                std::cout << "\n";
            }
        }
        return true;
    }
    if (line == "/multi") {
        return false;
    }
    return false;
}

} // namespace

void run_repl(AgentLoop &agent, SessionStore &store, const std::string &model, const Config &cfg, std::string &current_session_id, const std::string &agent_key,
              const std::string &scope_key, const SkillLoader *skill_loader, const ToolRegistry *tool_registry, HookManager *hook_manager) {
    std::cout << "Orangutan v0.1.0\n";
    std::cout << "Type /help for commands, Ctrl+D to quit\n\n";

    dispatch_session_start(hook_manager, current_session_id, agent.history().size());

    while (true) {
        auto maybe_line = read_input_line();
        if (!maybe_line.has_value()) {
            break;
        }

        auto line = std::move(*maybe_line);
        if (line.empty()) {
            continue;
        }

        bool quit = false;
        if (line[0] == '/' &&
            handle_slash_command(line, agent, store, model, current_session_id, quit, cfg, agent_key, scope_key, skill_loader, tool_registry, hook_manager)) {
            if (quit) {
                break;
            }
            continue;
        }

        if (line == "/multi") {
            line = read_multiline();
            if (line.empty()) {
                continue;
            }
        }

        try {
            agent.run(line);
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << "\n\n";
        }
    }

    if (cfg.auto_save && !agent.history().empty()) {
        if (!current_session_id.empty()) {
            store.update(current_session_id, agent.history());
            std::cout << "\nSession updated: " << current_session_id << " (use -r " << current_session_id << " to resume)\n";
        } else {
            current_session_id = store.save(agent.history(), model, scope_key);
            dispatch_session_start(hook_manager, current_session_id, agent.history().size());
            std::cout << "\nAuto-saved session: " << current_session_id << " (use -r " << current_session_id << " to resume)\n";
        }
    }

    dispatch_session_end(hook_manager, current_session_id, agent.history().size());

    std::cout << "\nBye!\n";
}

} // namespace orangutan::app
