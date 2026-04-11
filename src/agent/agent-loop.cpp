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

namespace orangutan::agent {

    AgentLoop::AgentLoop(Provider &provider, ToolRegistry &tools, RuntimeMemory *memory, std::string skills_prompt, HookManager *hook_manager)
    : provider_(&provider),
      tools_(&tools),
      memory_(memory),
      skills_prompt_(std::move(skills_prompt)),
      hook_manager_(hook_manager) {}

    std::string AgentLoop::run(const std::string &user_input, const StreamCallback &on_stream_event, const ToolEventCallback &on_tool_event,
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

        for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
            spdlog::debug("Agent loop iteration {}", iteration + 1);

            static_cast<void>(inject_incoming_messages(on_history_checkpoint));
            if (stop_requested()) {
                final_text = "Task terminated.";
                break;
            }

            auto tool_defs = tools_->definitions();
            const auto effective_system_prompt = detail::build_system_prompt(env_info_, user_input, skills_prompt_, *tools_, memory_);

            bool first_text = true;
            auto callback = detail::make_stream_callback(first_text, human_output, on_stream_event);
            auto response = provider_->chat_stream(effective_system_prompt, history_, tool_defs, callback, 4096, thinking_budget_);

            auto parts = detail::split_response(response);
            final_text += parts.text;
            history_.emplace_back(base::role::assistant, std::move(parts.all_blocks));
            detail::emit_history_checkpoint(on_history_checkpoint, history_);

            if (parts.tool_calls.empty() || response.stop_reason == "end_turn") {
                bool continuation_produced_tool_calls = false;
                if (response.stop_reason == "max_tokens" && parts.tool_calls.empty()) {
                    auto continuation = detail::handle_continuation(*provider_, *tools_, history_, effective_system_prompt, first_text, human_output, on_stream_event,
                                                                    thinking_budget_, on_history_checkpoint);
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

            auto [result_blocks, status] = detail::execute_tools(parts.tool_calls, *tools_, call_counts, hook_manager_, human_output, on_tool_event);
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
        return detail::compact_history(*provider_, history_, detail::COMPACTION_KEEP_RECENT + 1);
    }

    AgentLoop::SessionMemoryDistillationResult AgentLoop::distill_session_memory() {
        return detail::distill_session_memory(*provider_, memory_, history_);
    }

} // namespace orangutan::agent
