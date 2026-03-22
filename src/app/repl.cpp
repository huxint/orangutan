#include "app/repl.hpp"

#include "app/cli-ui.hpp"
#include "app/session-workflow.hpp"
#include "app/slash-commands.hpp"
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
    if (result.compacted && !current_session_id.empty()) {
        store.update(current_session_id, agent.history(), make_cli_session_metadata(model, scope_key, agent_key));
    }

    std::print("{}\n\n", format_history_compaction_result(result));
}

void export_session(AgentLoop &agent, SessionStore &store, std::string &current_session_id, const std::string &model, const std::string &scope_key, const std::string &agent_key,
                    const std::string &workspace_root, HookManager *hook_manager) {
    if (current_session_id.empty() && !agent.history().empty() && persist_session(agent, store, current_session_id, make_cli_session_metadata(model, scope_key, agent_key))) {
        dispatch_session_start(hook_manager, current_session_id, agent.history().size());
    }

    std::print("{}\n\n", describe_export_result(export_session_markdown(agent.history(), current_session_id, workspace_root)));
}

bool handle_slash_command(const std::string &line, AgentLoop &agent, const Provider &provider, SessionStore &store, const std::string &configured_model,
                          const std::vector<std::string> &fallback_models, std::string &current_session_id, bool &quit, const Config &cfg, const std::string &agent_key,
                          const std::string &scope_key, const std::string &workspace_root, const SkillLoader *skill_loader, const ToolRegistry *tool_registry,
                          HookManager *hook_manager) {
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
    if (line == "/export") {
        export_session(agent, store, current_session_id, active_model, scope_key, agent_key, workspace_root, hook_manager);
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
        std::print("{}\n\n", format_current_agent(agent_key));
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
    if (line.starts_with("/resume ")) {
        const auto session_id = line.substr(8);
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
    if (const auto reply = handle_registry_slash_command(line, tool_registry); reply.handled) {
        std::print("{}\n\n", reply.text);
        return true;
    }
    if (line == "/multi") {
        return false;
    }
    return false;
}

} // namespace

void run_repl(AgentLoop &agent, const Provider &provider, SessionStore &store, const std::string &configured_model, const std::vector<std::string> &fallback_models,
              const Config &cfg, std::string &current_session_id, const std::string &agent_key, const std::string &scope_key, const std::string &workspace_root,
              const SkillLoader *skill_loader, const ToolRegistry *tool_registry, HookManager *hook_manager, automation::Runtime *automation_runtime) {
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
                                                       workspace_root, skill_loader, tool_registry, hook_manager)) {
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
