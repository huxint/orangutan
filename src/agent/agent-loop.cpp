#include "agent/agent-loop.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/color.h>
#include <spdlog/spdlog.h>

#include "agent/history-compaction.hpp"
#include "agent/session-memory.hpp"
#include "agent/tool-executor.hpp"
#include "hooks/hook-manager.hpp"
#include "prompt/prompt-compiler.hpp"
#include "skills/skill-loader.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

namespace orangutan::agent {

    namespace {

        constexpr int MAX_CONTINUATIONS = 3;

        struct TurnContext {
            std::string memory_section;
            std::string final_text;
            ToolExecutionState tool_execution;
            bool human_output = false;
        };

        struct ResponseParts {
            std::vector<Content> all_blocks;
            std::vector<ToolUse> tool_calls;
            std::string text;
        };

        struct ContinuationResult {
            std::string appended_text;
            std::vector<ToolUse> tool_calls;
        };

        void write_stdout(std::string_view text) {
            if (!text.empty()) {
                static_cast<void>(std::fwrite(text.data(), sizeof(char), text.size(), stdout));
            }
            std::fflush(stdout);
        }

        void emit_history_checkpoint(const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint, const std::vector<Message> &history) {
            if (on_history_checkpoint != nullptr) {
                on_history_checkpoint(history);
            }
        }

        [[nodiscard]]
        std::string merge_refreshed_skill_prompt(std::string_view base_prompt, std::string_view refreshed_skill_section) {
            constexpr std::string_view SKILL_SECTION_MARKER = "\n\n## Available Skills\n";
            const auto marker_pos = base_prompt.find(SKILL_SECTION_MARKER);
            const auto prefix = marker_pos == std::string_view::npos ? base_prompt : base_prompt.substr(0, marker_pos);

            std::string merged(prefix);
            merged.append(refreshed_skill_section);
            return merged;
        }

        [[nodiscard]]
        std::string effective_skills_prompt(std::string_view base_prompt, skills::SkillLoader *skill_loader) {
            if (skill_loader == nullptr) {
                return std::string(base_prompt);
            }
            const auto refreshed_skill_section = skills::render_skill_prompt_section_or_fallback(skill_loader->list(skills::skill_list_query{.include_inactive = false}));
            return merge_refreshed_skill_prompt(base_prompt, refreshed_skill_section);
        }

        [[nodiscard]]
        std::string build_system_prompt(std::string_view default_system_prompt, std::string_view skills_prompt, const ToolRegistry &tools, std::string_view memory_section) {
            std::vector<prompt::PromptSection> sections;
            sections.push_back(prompt::PromptSection{
                .id = "system.default",
                .kind = prompt::prompt_section_kind::static_section,
                .classification = prompt::prompt_section_classification::must_keep,
                .priority = 100,
                .content = std::string(default_system_prompt),
                .cache_key_part = "system.default",
            });
            if (!skills_prompt.empty()) {
                sections.push_back(prompt::PromptSection{
                    .id = "system.skills",
                    .kind = prompt::prompt_section_kind::dynamic_section,
                    .classification = prompt::prompt_section_classification::important,
                    .priority = 90,
                    .content = std::string(skills_prompt),
                    .cache_key_part = "system.skills",
                });
            }

            const auto deferred = tools.deferred_tool_summaries();
            if (!deferred.empty()) {
                std::string deferred_section = "<available-deferred-tools>\n";
                deferred_section += "The following tools are already registered in the running process but their full schemas are deferred. "
                                    "Use the `tool_search` tool to discover and enable them before use; do not use shell commands to search configuration files for them.\n";
                for (const auto &tool : deferred) {
                    deferred_section += tool.name;
                    deferred_section.push_back('\n');
                }
                deferred_section += "</available-deferred-tools>";

                sections.push_back(prompt::PromptSection{
                    .id = "system.deferred_tools",
                    .kind = prompt::prompt_section_kind::dynamic_section,
                    .classification = prompt::prompt_section_classification::important,
                    .priority = 80,
                    .content = std::move(deferred_section),
                    .cache_key_part = "system.deferred_tools",
                });
            }

            auto prompt = prompt::compile_prompt(prompt::PromptBuildInput{
                                                     .sections = std::move(sections),
                                                     .token_budget = 0,
                                                 })
                              .full_prompt;

            if (memory_section.empty()) {
                return prompt;
            }
            prompt.append(memory_section);
            return prompt;
        }

        [[nodiscard]]
        AgentLoop::ProviderEventCallback make_stream_callback(bool &first_text, bool human_output, const AgentLoop::ProviderEventCallback &on_event) {
            return [&first_text, human_output, &on_event](const ProviderEvent &event) {
                if (const auto *thinking = std::get_if<ThinkingDelta>(&event)) {
                    if (human_output && first_text) {
                        std::string prompt;
                        utils::format_to(prompt, "\n");
                        utils::format_to(prompt, fmt::fg(fmt::terminal_color::green), "{}", "orangutan> ");
                        write_stdout(prompt);
                        first_text = false;
                    }
                    if (human_output) {
                        std::string rendered_thinking;
                        utils::format_to(rendered_thinking, fmt::fg(fmt::terminal_color::bright_black), "{}", thinking->thinking);
                        write_stdout(rendered_thinking);
                    }
                } else if (const auto *text = std::get_if<TextDelta>(&event)) {
                    if (human_output && first_text) {
                        std::string prompt;
                        utils::format_to(prompt, "\n");
                        utils::format_to(prompt, fmt::fg(fmt::terminal_color::green), "{}", "orangutan> ");
                        write_stdout(prompt);
                        first_text = false;
                    }
                    if (human_output) {
                        write_stdout(text->text);
                    }
                }
                if (on_event != nullptr) {
                    on_event(event);
                }
            };
        }

        [[nodiscard]]
        ResponseParts split_response(const LLMResponse &response) {
            ResponseParts parts;
            for (const auto &block : response.content) {
                parts.all_blocks.push_back(block);
                if (const auto *text = std::get_if<Text>(&block)) {
                    parts.text += text->text;
                } else if (const auto *tool = std::get_if<ToolUse>(&block)) {
                    parts.tool_calls.push_back(*tool);
                }
            }
            return parts;
        }

        [[nodiscard]]
        ContinuationResult handle_continuation(ProviderSystem &provider, const ProviderRoute &route, ToolRegistry &tools, std::vector<Message> &history,
                                               const std::string &system_prompt, bool &first_text, bool human_output, const AgentLoop::ProviderEventCallback &on_stream_event,
                                               int thinking_budget, const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint) {
            ContinuationResult result;

            for (int attempt = 0; attempt < MAX_CONTINUATIONS; ++attempt) {
                spdlog::debug("max-token continuation attempt {}", attempt + 1);

                history.push_back(Message::user().text("Please continue from where you left off."));
                emit_history_checkpoint(on_history_checkpoint, history);

                auto tool_defs = tools.definitions();
                auto callback = make_stream_callback(first_text, human_output, on_stream_event);
                const auto provider_result = provider.route(route)
                                                 .system(system_prompt)
                                                 .messages(history)
                                                 .tools(tool_defs)
                                                 .max_tokens(4096)
                                                 .thinking_budget(thinking_budget)
                                                 .stream()
                                                 .on_event(callback)
                                                 .send_blocking();
                auto response = provider_result.response;

                auto parts = split_response(response);
                result.appended_text += parts.text;
                history.emplace_back(base::role::assistant, parts.all_blocks);
                emit_history_checkpoint(on_history_checkpoint, history);

                if (!parts.tool_calls.empty()) {
                    result.tool_calls = std::move(parts.tool_calls);
                    break;
                }

                if (response.stop_reason != response_stop_reason::max_tokens) {
                    break;
                }
            }

            return result;
        }

    } // namespace

    AgentLoop::AgentLoop(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools, memory::RuntimeMemory *memory, std::string skills_prompt, HookManager *hook_manager,
                         skills::SkillLoader *skill_loader)
    : provider_(&provider),
      provider_route_(std::move(route)),
      tools_(&tools),
      memory_(memory),
      skills_prompt_(std::move(skills_prompt)),
      hook_manager_(hook_manager),
      skill_loader_(skill_loader) {
        refresh_default_system_prompt();
    }

    std::string AgentLoop::run(const std::string &user_input, const ProviderEventCallback &on_stream_event, const ToolEventCallback &on_tool_event,
                               const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint) {
        if (hook_manager_ != nullptr) {
            const auto context = build_message_context(hook_event::message_received, "user", user_input);
            static_cast<void>(hook_manager_->dispatch(hook_event::message_received, context));
        }

        history_.push_back(Message::user().text(user_input));
        emit_history_checkpoint(on_history_checkpoint, history_);

        TurnContext turn{
            .memory_section = render_prompt_memory_section(memory_, user_input),
            .human_output = on_stream_event == nullptr && on_tool_event == nullptr,
        };

        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
            spdlog::debug("agent loop iteration {}", iteration + 1);

            const auto skills_prompt = effective_skills_prompt(skills_prompt_, skill_loader_);

            static_cast<void>(inject_incoming_messages(on_history_checkpoint));
            if (stop_requested()) {
                turn.final_text = "Task terminated.";
                break;
            }

            auto tool_defs = tools_->definitions();
            const auto effective_system_prompt = build_system_prompt(default_system_prompt_, skills_prompt, *tools_, turn.memory_section);

            bool first_text = true;
            auto callback = make_stream_callback(first_text, turn.human_output, on_stream_event);
            auto response = provider_->route(provider_route_)
                                .system(effective_system_prompt)
                                .messages(history_)
                                .tools(tool_defs)
                                .max_tokens(4096)
                                .thinking_budget(thinking_budget_)
                                .stream()
                                .on_event(callback)
                                .send_blocking()
                                .response;

            auto parts = split_response(response);
            turn.final_text += parts.text;
            history_.emplace_back(base::role::assistant, std::move(parts.all_blocks));
            emit_history_checkpoint(on_history_checkpoint, history_);

            if (parts.tool_calls.empty() || response.stop_reason == response_stop_reason::end_turn) {
                bool continuation_produced_tool_calls = false;
                if (response.stop_reason == response_stop_reason::max_tokens && parts.tool_calls.empty()) {
                    auto continuation = handle_continuation(*provider_, provider_route_, *tools_, history_, effective_system_prompt, first_text, turn.human_output, on_stream_event,
                                                            thinking_budget_, on_history_checkpoint);
                    turn.final_text += continuation.appended_text;
                    if (!continuation.tool_calls.empty()) {
                        parts.tool_calls = std::move(continuation.tool_calls);
                        continuation_produced_tool_calls = true;
                    }
                }
                if (!continuation_produced_tool_calls) {
                    if (turn.human_output && !first_text) {
                        static_cast<void>(std::fputs("\n\n", stdout));
                        std::fflush(stdout);
                    }
                    break;
                }
            }

            if (turn.human_output && !first_text) {
                static_cast<void>(std::fputc('\n', stdout));
                std::fflush(stdout);
            }

            if (stop_requested()) {
                turn.final_text = "Task terminated.";
                break;
            }

            auto tool_execution = turn.tool_execution.execute(parts.tool_calls, *tools_, hook_manager_, turn.human_output, on_tool_event, skill_loader_);
            history_.emplace_back(base::role::user, std::move(tool_execution.result_blocks));
            emit_history_checkpoint(on_history_checkpoint, history_);

            if (tool_execution.status == tool_loop_status::abort) {
                turn.final_text = "I got stuck in a loop repeating the same action. Please try rephrasing your request.";
                history_.push_back(Message::assistant().text(turn.final_text));
                emit_history_checkpoint(on_history_checkpoint, history_);
                break;
            }
            if (tool_execution.status == tool_loop_status::warning) {
                history_.push_back(Message::user().text("You are repeating the same tool call with the same arguments. "
                                                        "This is not making progress. Try a different approach or "
                                                        "explain what you're trying to accomplish."));
                emit_history_checkpoint(on_history_checkpoint, history_);
            }

            turn.final_text.clear();
        }

        if (hook_manager_ != nullptr && !turn.final_text.empty()) {
            const auto context = build_message_context(hook_event::message_sending, "assistant", turn.final_text);
            static_cast<void>(hook_manager_->dispatch(hook_event::message_sending, context));
        }

        return turn.final_text;
    }

    bool AgentLoop::inject_incoming_messages(const HistoryCheckpointCallback &on_history_checkpoint) {
        if (!incoming_message_fetcher_) {
            return false;
        }

        auto messages = incoming_message_fetcher_();
        if (messages.empty()) {
            return false;
        }

        for (auto &message : messages) {
            history_.push_back(Message::user().text(message));
            emit_history_checkpoint(on_history_checkpoint, history_);
        }
        return true;
    }

    void AgentLoop::refresh_default_system_prompt() {
        default_system_prompt_ = prompt::build_default_system_prompt(env_info_);
    }

    bool AgentLoop::stop_requested() const {
        return stop_requested_callback_ != nullptr && stop_requested_callback_();
    }

    void AgentLoop::clear_history() {
        history_.clear();
        tools_->clear_discovered();
    }

    AgentLoop::HistoryCompactionResult AgentLoop::compress_history() {
        return compact_conversation_history(*provider_, provider_route_, history_);
    }

    AgentLoop::SessionMemoryDistillationResult AgentLoop::distill_session_memory() {
        return distill_session_memory_from_history(*provider_, provider_route_, memory_, history_);
    }

} // namespace orangutan::agent
