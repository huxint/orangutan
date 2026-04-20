#pragma once

#include <cstdio>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/fmt/bundled/color.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

#include "agent/agent-loop.hpp"
#include "utils/format.hpp"

namespace orangutan::agent::detail {

    inline constexpr int MAX_CONTINUATIONS = 3;
    inline constexpr int COMPACTION_KEEP_RECENT = 10;

    struct ResponseParts {
        std::vector<Content> all_blocks;
        std::vector<ToolUse> tool_calls;
        std::string text;
    };

    struct ContinuationResult {
        std::string appended_text;
        std::vector<ToolUse> tool_calls;
    };

    inline void write_stdout(std::string_view text) {
        if (!text.empty()) {
            static_cast<void>(std::fwrite(text.data(), sizeof(char), text.size(), stdout));
        }
        std::fflush(stdout);
    }

    inline void emit_history_checkpoint(const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint, const std::vector<Message> &history) {
        if (on_history_checkpoint != nullptr) {
            on_history_checkpoint(history);
        }
    }

    [[nodiscard]]
    inline AgentLoop::ProviderEventCallback make_stream_callback(bool &first_text, bool human_output, const AgentLoop::ProviderEventCallback &on_event) {
        return [&first_text, human_output, &on_event](const ProviderEvent &event) {
            if (const auto *thinking = std::get_if<ThinkingDelta>(&event)) {
                if (human_output && first_text) {
                    std::string prompt;
                    utils::format_to(prompt, "\n");
                    utils::format_to(prompt, spdlog::fmt_lib::fg(spdlog::fmt_lib::terminal_color::green), "{}", "orangutan> ");
                    write_stdout(prompt);
                    first_text = false;
                }
                if (human_output) {
                    std::string rendered_thinking;
                    utils::format_to(rendered_thinking, spdlog::fmt_lib::fg(spdlog::fmt_lib::terminal_color::bright_black), "{}", thinking->thinking);
                    write_stdout(rendered_thinking);
                }
            } else if (const auto *text = std::get_if<TextDelta>(&event)) {
                if (human_output && first_text) {
                    std::string prompt;
                    utils::format_to(prompt, "\n");
                    utils::format_to(prompt, spdlog::fmt_lib::fg(spdlog::fmt_lib::terminal_color::green), "{}", "orangutan> ");
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
    inline ResponseParts split_response(const LLMResponse &response) {
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
    inline ContinuationResult handle_continuation(ProviderSystem &provider, const ProviderRoute &route, ToolRegistry &tools, std::vector<Message> &history,
                                                  const std::string &system_prompt, bool &first_text, bool human_output,
                                                  const AgentLoop::ProviderEventCallback &on_stream_event, int thinking_budget,
                                                  const AgentLoop::HistoryCheckpointCallback &on_history_checkpoint) {
        ContinuationResult result;

        for (int attempt = 0; attempt < MAX_CONTINUATIONS; ++attempt) {
            spdlog::debug("max-token continuation attempt {}", attempt + 1);

            history.push_back(Message::user().text("Please continue from where you left off."));
            emit_history_checkpoint(on_history_checkpoint, history);

            auto tool_defs = tools.definitions();
            auto callback = make_stream_callback(first_text, human_output, on_stream_event);
            const auto provider_result =
                provider.route(route).system(system_prompt).messages(history).tools(tool_defs).max_tokens(4096).thinking_budget(thinking_budget).stream().on_event(callback).send_blocking();
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

    [[nodiscard]]
    inline AgentLoop::HistoryCompactionResult compact_history(ProviderSystem &provider, const ProviderRoute &route, std::vector<Message> &history,
                                                              std::size_t minimum_history_size) {
        AgentLoop::HistoryCompactionResult result{
            .compacted = false,
            .messages_before = history.size(),
            .messages_after = history.size(),
        };
        if (history.size() < minimum_history_size) {
            result.status = "Not enough history to compress yet.";
            return result;
        }

        const auto keep_start = static_cast<int>(history.size()) - COMPACTION_KEEP_RECENT;
        if (keep_start <= 0) {
            result.status = "Not enough history to compress yet.";
            return result;
        }

        spdlog::info("compacting history: {} messages -> summarizing first {}, keeping last {}", history.size(), keep_start, COMPACTION_KEEP_RECENT);

        std::vector<Message> older_messages(history.begin(), history.begin() + keep_start);
        constexpr std::string_view SUMMARY_PROMPT = "You are a conversation summarizer. Summarize the following conversation "
                                                    "concisely, preserving key facts, decisions, and context that would be "
                                                    "needed to continue the conversation. Focus on what was discussed, what "
                                                    "tools were used, and what results were obtained.";

        std::vector<ToolDef> no_tools;
        try {
            auto response = provider.route(route).system(SUMMARY_PROMPT).messages(older_messages).tools(no_tools).max_tokens(1024).send_blocking().response;

            std::string summary_text;
            for (const auto &block : response.content) {
                if (const auto *text = std::get_if<Text>(&block)) {
                    summary_text += text->text;
                }
            }

            if (summary_text.empty()) {
                spdlog::warn("compaction produced empty summary, skipping");
                result.status = "Compaction produced an empty summary.";
                return result;
            }

            std::vector<Message> compacted;
            compacted.push_back(Message::user().text("[Conversation summary]\n" + summary_text));
            compacted.insert(compacted.end(), history.begin() + keep_start, history.end());
            history = std::move(compacted);

            spdlog::info("history compacted to {} messages", history.size());
            result.compacted = true;
            result.messages_after = history.size();
            result.status = "History compressed.";
        } catch (const std::exception &e) {
            spdlog::warn("history compaction failed: {}", e.what());
            result.status = std::string("History compaction failed: ") + e.what();
        }
        return result;
    }

} // namespace orangutan::agent::detail
