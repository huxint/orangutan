#include "app/repl.hpp"

#include "app/cli-ui.hpp"
#include "app/session-workflow.hpp"
#include "core/providers/provider.hpp"
#include "features/automation/runtime.hpp"
#include "features/hooks/hook-manager.hpp"
#include "infra/storage/session-store.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <ranges>
#include <string_view>

#include <readline/history.h>
#include <readline/readline.h>

namespace orangutan::app {

namespace {

using ReadlinePtr = std::unique_ptr<char, decltype(&std::free)>;

std::string read_multiline() {
    std::println("Multi-line mode. Enter an empty line to finish.");
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

void save_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key, const std::string &agent_key,
                  HookManager *hook_manager) {
    const auto updating_existing = !current_session_id.empty();
    if (!persist_session(agent, store, current_session_id, make_cli_session_metadata(model, scope_key, agent_key))) {
        std::print("💤 Nothing to save (empty history).\n\n");
        return;
    }

    if (!updating_existing) {
        dispatch_session_start(hook_manager, current_session_id, agent.history().size());
    }

    if (updating_existing) {
        std::print("💾 Session updated: {} (use -r {} to resume)\n\n", current_session_id, current_session_id);
        return;
    }
    std::print("💾 Session saved: {} (use -r {} to resume)\n\n", current_session_id, current_session_id);
}

void load_session(const std::string &requested_session_id, AgentLoop &agent, SessionStore &store, std::string &current_session_id, const std::string &scope_key,
                  const std::string &agent_key, HookManager *hook_manager) {
    const auto previous_session_id = current_session_id;
    const auto previous_message_count = agent.history().size();
    const auto result = load_session_into_agent(requested_session_id, agent, store, current_session_id, scope_key, agent_key);
    if (result.loaded && current_session_id != previous_session_id) {
        dispatch_session_end(hook_manager, previous_session_id, previous_message_count);
        dispatch_session_start(hook_manager, current_session_id, agent.history().size());
    }
    std::print("{}\n\n", result.status);
}

void compress_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const std::string &model, const std::string &scope_key,
                      const std::string &agent_key) {
    const auto result = agent.compress_history();
    if (!result.compacted) {
        std::print("{}\n\n", result.status);
        return;
    }

    if (!current_session_id.empty()) {
        store.update(current_session_id, agent.history(), make_cli_session_metadata(model, scope_key, agent_key));
    }

    std::print("🗜️ Compressed history: {} -> {} messages.\n\n", result.messages_before, result.messages_after);
}

std::string trim_copy(std::string_view input) {
    const auto begin = input.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = input.find_last_not_of(" \t");
    return std::string(input.substr(begin, end - begin + 1));
}

bool execute_registry_command(const ToolRegistry *tool_registry, std::string_view tool_name, const json &input, std::string_view tool_use_id) {
    if (tool_registry == nullptr) {
        std::print("No tool registry available.\n\n");
        return true;
    }

    const auto result = tool_registry->execute(ToolUseBlock{
        .id = std::string(tool_use_id),
        .name = std::string(tool_name),
        .input = input,
    });
    std::print("{}\n\n", result.content);
    return true;
}

bool handle_tasks_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/tasks") {
        return execute_registry_command(tool_registry, "task", {{"op", "list"}}, "slash-task-list");
    }
    if (!line.starts_with("/tasks ")) {
        return false;
    }

    const auto remainder = trim_copy(std::string_view(line).substr(7));
    if (remainder.starts_with("run ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(4));
        if (id.empty()) {
            std::print("Usage: /tasks run <id>\n\n");
            return true;
        }
        return execute_registry_command(tool_registry, "task", {{"op", "run"}, {"id", id}}, "slash-task-run");
    }
    if (remainder.starts_with("remove ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(7));
        if (id.empty()) {
            std::print("Usage: /tasks remove <id>\n\n");
            return true;
        }
        return execute_registry_command(tool_registry, "task", {{"op", "remove"}, {"id", id}}, "slash-task-remove");
    }

    std::print("Usage: /tasks | /tasks run <id> | /tasks remove <id>\n\n");
    return true;
}

bool handle_heartbeats_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/heartbeats") {
        return execute_registry_command(tool_registry, "heartbeat", {{"op", "list"}}, "slash-heartbeat-list");
    }
    if (!line.starts_with("/heartbeats ")) {
        return false;
    }

    const auto remainder = trim_copy(std::string_view(line).substr(12));
    const auto run_action = [&](std::string_view action, std::string_view op, std::string_view tool_use_id) {
        if (!remainder.starts_with(action)) {
            return false;
        }
        const auto id = trim_copy(std::string_view(remainder).substr(action.size()));
        if (id.empty()) {
            std::print("Usage: /heartbeats {} <id>\n\n", op);
            return true;
        }
        return execute_registry_command(tool_registry, "heartbeat", {{"op", std::string(op)}, {"id", id}}, std::string(tool_use_id));
    };

    if (run_action("run ", "run", "slash-heartbeat-run")) {
        return true;
    }
    if (run_action("pause ", "pause", "slash-heartbeat-pause")) {
        return true;
    }
    if (run_action("resume ", "resume", "slash-heartbeat-resume")) {
        return true;
    }
    if (run_action("remove ", "remove", "slash-heartbeat-remove")) {
        return true;
    }

    std::print("Usage: /heartbeats | /heartbeats run <id> | /heartbeats pause <id> | /heartbeats resume <id> | /heartbeats remove <id>\n\n");
    return true;
}

bool handle_inbox_command(const std::string &line, const ToolRegistry *tool_registry) {
    if (line == "/inbox") {
        return execute_registry_command(tool_registry, "inbox", {{"op", "list"}}, "slash-inbox-list");
    }
    if (line == "/inbox clear") {
        return execute_registry_command(tool_registry, "inbox", {{"op", "clear"}}, "slash-inbox-clear");
    }
    if (!line.starts_with("/inbox ")) {
        return false;
    }

    const auto remainder = trim_copy(std::string_view(line).substr(7));
    if (remainder.starts_with("ack ")) {
        const auto id = trim_copy(std::string_view(remainder).substr(4));
        if (id.empty()) {
            std::print("Usage: /inbox ack <id>\n\n");
            return true;
        }
        return execute_registry_command(tool_registry, "inbox", {{"op", "ack"}, {"id", id}}, "slash-inbox-ack");
    }

    std::print("Usage: /inbox | /inbox ack <id> | /inbox clear\n\n");
    return true;
}

bool handle_slash_command(const std::string &line, AgentLoop &agent, const Provider &provider, SessionStore &store, const std::string &configured_model,
                          const std::vector<std::string> &fallback_models, std::string &current_session_id, bool &quit, const Config &cfg, const std::string &agent_key,
                          const std::string &scope_key, const SkillLoader *skill_loader, const ToolRegistry *tool_registry, HookManager *hook_manager) {
    const auto active_model = provider.current_model().empty() ? configured_model : provider.current_model();

    if (line == "/quit" || line == "/exit") {
        quit = true;
        return true;
    }
    if (line == "/help") {
        std::print("{}", repl_help_text());
        return true;
    }
    if (line == "/new") {
        const auto previous_message_count = agent.history().size();
        const auto result = start_new_session(agent, store, current_session_id, make_cli_session_metadata(active_model, scope_key, agent_key));
        dispatch_session_end(hook_manager, result.previous_session_id, previous_message_count);
        std::print("{}\n\n", describe_new_session_result(result, false));
        return true;
    }
    if (line == "/compress") {
        compress_session(agent, store, current_session_id, active_model, scope_key, agent_key);
        return true;
    }
    if (line == "/clear") {
        agent.clear_history();
        std::print("History cleared.\n\n");
        return true;
    }
    if (line == "/history") {
        std::print("{}", render_history_summary(agent));
        return true;
    }
    if (line == "/session") {
        if (current_session_id.empty()) {
            std::print("💤 No active session.\n\n");
        } else {
            std::print("🧵 Current session: {}\n\n", current_session_id);
        }
        return true;
    }
    if (line == "/save") {
        save_session(agent, store, active_model, current_session_id, scope_key, agent_key, hook_manager);
        return true;
    }
    if (line == "/sessions") {
        std::print("{}", render_saved_sessions(store, scope_key));
        return true;
    }
    if (line == "/agent") {
        std::print("🤖 Current agent: {}\n\n", agent_key);
        return true;
    }
    if (line == "/status") {
        std::print("{}\n\n",
                   format_runtime_status(collect_runtime_status(agent, provider, tool_registry, current_session_id, agent_key, configured_model, fallback_models, scope_key)));
        return true;
    }
    if (line == "/agents") {
        std::print("{}\n\n", format_agent_list(cfg, agent_key));
        return true;
    }
    if (line.starts_with("/load ") || line.starts_with("/resume ")) {
        const auto session_id = line.starts_with("/resume ") ? line.substr(8) : line.substr(6);
        load_session(session_id, agent, store, current_session_id, scope_key, agent_key, hook_manager);
        return true;
    }
    if (line == "/skills") {
        if (skill_loader == nullptr || skill_loader->active_skills().empty()) {
            std::print("No skills loaded.\n\n");
        } else {
            std::println("Loaded skills:");
            for (const auto &skill : skill_loader->active_skills()) {
                std::println("  {} — {}", skill.name, skill.description);
                std::println("    source: {}", skill.source_path);
                if (!skill.tools.empty()) {
                    std::print("    tools: ");
                    auto joined_tools = skill.tools | std::views::transform([](const std::string &tool) -> std::string_view {
                                            return tool;
                                        }) |
                                        std::views::join_with(std::string_view{", "});
                    std::ranges::copy(joined_tools, std::ostreambuf_iterator<char>(std::cout));
                    std::println("");
                }
            }
            std::println("");
        }
        return true;
    }
    if (line == "/tools") {
        if (tool_registry == nullptr) {
            std::print("No tool registry available.\n\n");
        } else {
            const auto defs = tool_registry->definitions();
            if (defs.empty()) {
                std::print("No tools registered.\n\n");
            } else {
                std::println("Registered tools ({}):", defs.size());
                for (const auto &def : defs) {
                    std::println("  {} — {}", def.name, def.description);
                }
                std::println("");
            }
        }
        return true;
    }
    if (handle_tasks_command(line, tool_registry)) {
        return true;
    }
    if (handle_heartbeats_command(line, tool_registry)) {
        return true;
    }
    if (handle_inbox_command(line, tool_registry)) {
        return true;
    }
    if (line == "/multi") {
        return false;
    }
    return false;
}

} // namespace

void run_repl(AgentLoop &agent, const Provider &provider, SessionStore &store, const std::string &configured_model, const std::vector<std::string> &fallback_models,
              const Config &cfg, std::string &current_session_id, const std::string &agent_key, const std::string &scope_key, const SkillLoader *skill_loader,
              const ToolRegistry *tool_registry, HookManager *hook_manager, automation::Runtime *automation_runtime) {
    std::println("Orangutan v0.1.0");
    std::print("Type /help for commands, Ctrl+D to quit\n\n");

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

        if (line == "/multi") {
            line = read_multiline();
            if (line.empty()) {
                continue;
            }
        }

        bool quit = false;
        const bool handled = automation::with_agent_execution_lease(automation_runtime, agent_key, [&] {
            if (line[0] == '/' && handle_slash_command(line, agent, provider, store, configured_model, fallback_models, current_session_id, quit, cfg, agent_key, scope_key,
                                                       skill_loader, tool_registry, hook_manager)) {
                return true;
            }

            try {
                agent.run(line);
            } catch (const std::exception &e) {
                std::print(std::cerr, "Error: {}\n\n", e.what());
            }
            return false;
        });
        if (quit) {
            break;
        }
        if (handled) {
            continue;
        }
    }

    if (cfg.auto_save && !agent.history().empty()) {
        const auto active_model = provider.current_model().empty() ? configured_model : provider.current_model();
        const auto metadata = make_cli_session_metadata(active_model, scope_key, agent_key);
        if (!current_session_id.empty()) {
            store.update(current_session_id, agent.history(), metadata);
            std::print("\n💾 Session updated: {} (use -r {} to resume)\n", current_session_id, current_session_id);
        } else {
            current_session_id = store.save(agent.history(), metadata);
            dispatch_session_start(hook_manager, current_session_id, agent.history().size());
            std::print("\n💾 Auto-saved session: {} (use -r {} to resume)\n", current_session_id, current_session_id);
        }
    }

    dispatch_session_end(hook_manager, current_session_id, agent.history().size());

    std::print("\nBye!\n");
}

} // namespace orangutan::app
