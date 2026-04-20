#include "agent/agent-loop.hpp"

#include <cstdio>
#include <string>

#include <spdlog/fmt/bundled/color.h>
#include <spdlog/spdlog.h>

#include "agent/agent-loop-history.hpp"
#include "agent/agent-loop-memory.hpp"
#include "agent/agent-loop-tools.hpp"
#include "hooks/hook-manager.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/skill-loader.hpp"

namespace orangutan::agent {

    namespace {

        [[nodiscard]]
        std::string merge_refreshed_skill_prompt(std::string_view base_prompt, std::string_view refreshed_skill_section) {
            constexpr std::string_view SKILL_SECTION_MARKER = "\n\n## Available Skills\n";
            const auto marker_pos = base_prompt.find(SKILL_SECTION_MARKER);
            const auto prefix = marker_pos == std::string_view::npos ? base_prompt : base_prompt.substr(0, marker_pos);

            std::string merged(prefix);
            merged.append(refreshed_skill_section);
            return merged;
        }

    } // namespace

    AgentLoop::AgentLoop(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools, RuntimeMemory *memory, std::string skills_prompt,
                         HookManager *hook_manager, skills::SkillLoader *skill_loader)
    : provider_(&provider),
      provider_route_(std::move(route)),
      tools_(&tools),
      memory_(memory),
      skills_prompt_(std::move(skills_prompt)),
      hook_manager_(hook_manager),
      skill_loader_(skill_loader) {}

    AgentLoopBuilder AgentLoop::configure(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools) {
        return AgentLoopBuilder(provider, std::move(route), tools);
    }

    std::string AgentLoop::run(const std::string &user_input, const ProviderEventCallback &on_stream_event, const ToolEventCallback &on_tool_event,
                               const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint) {
        detail::ToolCallCounts call_counts;

        if (hook_manager_ != nullptr) {
            const auto context = build_message_context(hook_event::message_received, "user", user_input);
            static_cast<void>(hook_manager_->dispatch(hook_event::message_received, context));
        }

        history_.push_back(Message::user().text(user_input));
        detail::emit_history_checkpoint(on_history_checkpoint, history_);

        std::string final_text;
        const bool human_output = !on_stream_event && !on_tool_event;

        const auto memory_section = detail::render_prompt_memory_section(memory_, user_input);

        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
            spdlog::debug("agent loop iteration {}", iteration + 1);

            const auto refreshed_skill_section =
                skill_loader_ == nullptr ? std::string{} : skills::render_skill_prompt_section(skill_loader_->list(skills::skill_list_query{.include_inactive = false}));
            const auto effective_skills_prompt = skill_loader_ == nullptr ? skills_prompt_ : merge_refreshed_skill_prompt(skills_prompt_, refreshed_skill_section);

            static_cast<void>(inject_incoming_messages(on_history_checkpoint));
            if (stop_requested()) {
                final_text = "Task terminated.";
                break;
            }

            auto tool_defs = tools_->definitions();
            const auto effective_system_prompt = detail::build_system_prompt(env_info_, effective_skills_prompt, *tools_, memory_section);

            bool first_text = true;
            auto callback = detail::make_stream_callback(first_text, human_output, on_stream_event);
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

            auto parts = detail::split_response(response);
            final_text += parts.text;
            history_.emplace_back(base::role::assistant, std::move(parts.all_blocks));
            detail::emit_history_checkpoint(on_history_checkpoint, history_);

            if (parts.tool_calls.empty() || response.stop_reason == response_stop_reason::end_turn) {
                bool continuation_produced_tool_calls = false;
                if (response.stop_reason == response_stop_reason::max_tokens && parts.tool_calls.empty()) {
                    auto continuation = detail::handle_continuation(*provider_, provider_route_, *tools_, history_, effective_system_prompt, first_text, human_output,
                                                                    on_stream_event, thinking_budget_, on_history_checkpoint);
                    final_text += continuation.appended_text;
                    if (!continuation.tool_calls.empty()) {
                        parts.tool_calls = std::move(continuation.tool_calls);
                        continuation_produced_tool_calls = true;
                    }
                }
                if (!continuation_produced_tool_calls) {
                    if (human_output && !first_text) {
                        static_cast<void>(std::fputs("\n\n", stdout));
                        std::fflush(stdout);
                    }
                    break;
                }
            }

            if (human_output && !first_text) {
                static_cast<void>(std::fputc('\n', stdout));
                std::fflush(stdout);
            }

            if (stop_requested()) {
                final_text = "Task terminated.";
                break;
            }

            auto [result_blocks, status] = detail::execute_tools(parts.tool_calls, *tools_, call_counts, hook_manager_, human_output, on_tool_event, skill_loader_);
            history_.emplace_back(base::role::user, std::move(result_blocks));
            detail::emit_history_checkpoint(on_history_checkpoint, history_);

            if (status == detail::loop_status::abort) {
                final_text = "I got stuck in a loop repeating the same action. Please try rephrasing your request.";
                history_.push_back(Message::assistant().text(final_text));
                detail::emit_history_checkpoint(on_history_checkpoint, history_);
                break;
            }
            if (status == detail::loop_status::warning) {
                history_.push_back(Message::user().text("You are repeating the same tool call with the same arguments. "
                                                        "This is not making progress. Try a different approach or "
                                                        "explain what you're trying to accomplish."));
                detail::emit_history_checkpoint(on_history_checkpoint, history_);
            }

            final_text.clear();
        }

        if (hook_manager_ != nullptr && !final_text.empty()) {
            const auto context = build_message_context(hook_event::message_sending, "assistant", final_text);
            static_cast<void>(hook_manager_->dispatch(hook_event::message_sending, context));
        }

        return final_text;
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
            detail::emit_history_checkpoint(on_history_checkpoint, history_);
        }
        return true;
    }

    bool AgentLoop::stop_requested() const {
        return stop_requested_callback_ != nullptr && stop_requested_callback_();
    }

    void AgentLoop::clear_history() {
        history_.clear();
        tools_->clear_discovered();
    }

    AgentLoop::HistoryCompactionResult AgentLoop::compress_history() {
        return detail::compact_history(*provider_, provider_route_, history_, detail::COMPACTION_KEEP_RECENT + 1);
    }

    AgentLoop::SessionMemoryDistillationResult AgentLoop::distill_session_memory() {
        return detail::distill_session_memory(*provider_, provider_route_, memory_, history_);
    }

} // namespace orangutan::agent
