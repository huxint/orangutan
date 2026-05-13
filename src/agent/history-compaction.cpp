#include "agent/history-compaction.hpp"

#include <cstddef>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace orangutan::agent {

    AgentLoop::HistoryCompactionResult compact_conversation_history(ProviderSystem &provider, const ProviderRoute &route, std::vector<Message> &history) {
        constexpr int COMPACTION_KEEP_RECENT = 10;
        constexpr std::size_t MINIMUM_HISTORY_SIZE = COMPACTION_KEEP_RECENT + 1;

        AgentLoop::HistoryCompactionResult result{
            .compacted = false,
            .messages_before = history.size(),
            .messages_after = history.size(),
        };
        if (history.size() < MINIMUM_HISTORY_SIZE) {
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

} // namespace orangutan::agent
