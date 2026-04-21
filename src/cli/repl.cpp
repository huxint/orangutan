#include "cli/repl.hpp"

#include "cli/cli-ui.hpp"
#include "cli/line-editor.hpp"
#include "cli/session-workflow.hpp"
#include "cli/slash-commands.hpp"
#include "providers/provider.hpp"
#include "automation/runtime.hpp"
#include "hooks/hook-manager.hpp"
#include "storage/session-store.hpp"

#include <cstdio>
#include <fmt/format.h>
#include <iostream>
#include <optional>
#include <string_view>
namespace orangutan::cli {

    namespace {

        void flush_repl_streams() {
            std::fflush(stdout);
            std::fflush(stderr);
        }

        void save_session(AgentLoop &agent, SessionStore &store, const std::string &model, std::string &current_session_id, const std::string &scope_key,
                          const std::string &agent_key, HookManager *hook_manager) {
            const auto updating_existing = !current_session_id.empty();
            if (!persist_session(agent, store, current_session_id, make_cli_session_metadata(model, scope_key, agent_key))) {
                fmt::println("💤 Nothing to save (empty history).\n");
                return;
            }

            if (!updating_existing) {
                dispatch_session_start(hook_manager, current_session_id, agent.history().size());
            }

            if (updating_existing) {
                fmt::println("💾 Session updated: {} (use -r {} to resume)\n", current_session_id, current_session_id);
                return;
            }
            fmt::println("💾 Session saved: {} (use -r {} to resume)\n", current_session_id, current_session_id);
        }

        void print_slash_reply(const std::string &text) {
            if (text.ends_with("\n\n")) {
                fmt::print("{}", text);
                return;
            }
            if (text.ends_with('\n')) {
                fmt::println("{}", text);
                return;
            }
            fmt::println("{}\n", text);
        }

        bool handle_slash_command(const std::string &line, AgentLoop &agent, const ProviderSystem &provider, SessionStore &store, const std::string &configured_model,
                                  const std::vector<std::string> &fallback_models, std::string &current_session_id, bool &quit, const Config &cfg, const std::string &agent_key,
                                  const std::string &scope_key, const std::string &workspace_root, const SkillLoader *skill_loader, const ToolRegistry *tool_registry,
                                  HookManager *hook_manager) {
            const auto active_model = provider.current_model().empty() ? configured_model : provider.current_model();

            if (line == "/quit" || line == "/exit") {
                quit = true;
                return true;
            }
            if (line == "/clear") {
                agent.clear_history();
                fmt::println("History cleared.\n");
                return true;
            }
            if (line == "/save") {
                save_session(agent, store, active_model, current_session_id, scope_key, agent_key, hook_manager);
                return true;
            }
            if (line == "/skills") {
                if (skill_loader == nullptr) {
                    fmt::println("No skills loaded.\n");
                } else {
                    print_slash_reply(format_skill_catalog(skill_loader->list(skills::skill_list_query{.include_inactive = true})));
                }
                return true;
            }
            if (line == "/tools") {
                if (tool_registry == nullptr) {
                    fmt::println("No tool registry available.\n");
                } else {
                    const auto defs = tool_registry->definitions();
                    if (defs.empty()) {
                        fmt::println("No tools registered.\n");
                    } else {
                        fmt::println("Registered tools ({}):", defs.size());
                        for (const auto &def : defs) {
                            fmt::println("  {} — {}", def.name, def.description);
                        }
                        fmt::println("");
                    }
                }
                return true;
            }
            if (const auto reply = dispatch_shared_slash_command(
                    line, {.surface = slash_command_surface::cli,
                           .help =
                               [] {
                                   return SlashCommandReply{.handled = true, .text = repl_help_text()};
                               },
                           .new_session =
                               [&] {
                                   const auto previous_message_count = agent.history().size();
                                   const auto result = start_new_session(agent, store, current_session_id, make_cli_session_metadata(active_model, scope_key, agent_key));
                                   dispatch_session_end(hook_manager, result.previous_session_id, previous_message_count);
                                   return SlashCommandReply{.handled = true, .text = describe_new_session_result(result, false)};
                               },
                           .export_session =
                               [&] {
                                   if (current_session_id.empty() && !agent.history().empty() &&
                                       persist_session(agent, store, current_session_id, make_cli_session_metadata(active_model, scope_key, agent_key))) {
                                       dispatch_session_start(hook_manager, current_session_id, agent.history().size());
                                   }
                                   return SlashCommandReply{
                                       .handled = true,
                                       .text = describe_export_result(export_session_markdown(agent.history(), current_session_id, workspace_root)),
                                   };
                               },
                           .compress =
                               [&] {
                                   const auto result = agent.compress_history();
                                   if (result.compacted && !current_session_id.empty()) {
                                       store.update(current_session_id, agent.history(), make_cli_session_metadata(active_model, scope_key, agent_key));
                                   }
                                   return SlashCommandReply{.handled = true, .text = format_history_compaction_result(result)};
                               },
                           .session =
                               [&] {
                                   return SlashCommandReply{.handled = true,
                                                            .text = current_session_id.empty() ? "💤 No active session.\n\n" : "🧵 Current session: " + current_session_id};
                               },
                           .sessions =
                               [&] {
                                   return SlashCommandReply{.handled = true, .text = render_saved_sessions(store, scope_key)};
                               },
                           .agent =
                               [&] {
                                   return SlashCommandReply{.handled = true, .text = format_current_agent(agent_key)};
                               },
                           .status =
                               [&] {
                                   return SlashCommandReply{
                                       .handled = true,
                                       .text = format_runtime_status(
                                           collect_runtime_status(agent, provider, tool_registry, current_session_id, agent_key, configured_model, fallback_models, scope_key)),
                                   };
                               },
                           .agents =
                               [&] {
                                   return SlashCommandReply{.handled = true, .text = format_agent_list(cfg, agent_key)};
                               },
                           .resume =
                               [&](const std::string &session_id) {
                                   const auto previous_session_id = current_session_id;
                                   const auto previous_message_count = agent.history().size();
                                   const auto result = load_session_into_agent(session_id, agent, store, current_session_id, scope_key, agent_key);
                                   if (result.loaded && current_session_id != previous_session_id) {
                                       dispatch_session_end(hook_manager, previous_session_id, previous_message_count);
                                       dispatch_session_start(hook_manager, current_session_id, agent.history().size());
                                   }
                                   return SlashCommandReply{.handled = true, .text = result.status};
                               },
                           .tool_registry = tool_registry});
                reply.handled) {
                print_slash_reply(reply.text);
                return true;
            }
            if (line == "/multi") {
                return false;
            }
            return false;
        }

    } // namespace

    void run_repl(AgentLoop &agent, const ProviderSystem &provider, SessionStore &store, const std::string &configured_model,
                  const std::vector<std::string> &fallback_models,
                  const Config &cfg, std::string &current_session_id, const std::string &agent_key, const std::string &scope_key, const std::string &workspace_root,
                  const SkillLoader *skill_loader, const ToolRegistry *tool_registry, HookManager *hook_manager,
                  automation::AutomationRuntime *automation_runtime) {
        fmt::println("Orangutan v0.1.0");
        fmt::println("Type /help for commands, Ctrl+D to quit\n");
        std::fflush(stdout);

        dispatch_session_start(hook_manager, current_session_id, agent.history().size());

        ReplxxLineEditor editor;
        while (true) {
            auto maybe_line = read_repl_input(editor);
            if (!maybe_line.has_value()) {
                break;
            }

            auto line = std::move(*maybe_line);
            if (line.empty()) {
                continue;
            }

            if (line == "/multi") {
                line = read_repl_multiline(editor, std::cout);
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
                    fmt::println(stderr, "Error: {}\n", e.what());
                }
                return false;
            });
            flush_repl_streams();
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
                fmt::println("\n💾 Session updated: {} (use -r {} to resume)", current_session_id, current_session_id);
            } else {
                current_session_id = store.save(agent.history(), metadata);
                dispatch_session_start(hook_manager, current_session_id, agent.history().size());
                fmt::println("\n💾 Auto-saved session: {} (use -r {} to resume)", current_session_id, current_session_id);
            }
        }

        dispatch_session_end(hook_manager, current_session_id, agent.history().size());

        fmt::println("\nBye!");
    }

} // namespace orangutan::cli
