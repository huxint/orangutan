#pragma once

#include "prompt/system-prompt-sections.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "types/base.hpp"

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::memory {
    class RuntimeMemory;
}

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::agent {

    class AgentLoop {
    public:
        using ToolEventCallback = std::function<void(const std::string &event_type, const ToolUse &call, const ToolResult *result)>;
        using ProviderEventCallback = std::function<void(const ProviderEvent &)>;
        using HistoryCheckpointCallback = std::function<void(const std::vector<Message> &history)>;
        using IncomingMessageFetcher = std::function<std::vector<std::string>()>;
        using StopRequestedCallback = std::function<bool()>;
        struct HistoryCompactionResult {
            bool compacted = false;
            std::size_t messages_before = 0;
            std::size_t messages_after = 0;
            std::string status;
        };

        struct SessionMemoryDistillationResult {
            bool distilled = false;
            std::size_t memories_stored = 0;
            bool journal_stored = false;
            std::string status;
        };

        AgentLoop(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools, memory::RuntimeMemory *memory = nullptr, std::string skills_prompt = {},
                  hooks::HookManager *hook_manager = nullptr,
                  skills::SkillLoader *skill_loader = nullptr);

        // Process one user message: run the ReAct loop until final text response
        std::string run(const std::string &user_input, const ProviderEventCallback &on_stream_event = {}, const ToolEventCallback &on_tool_event = {},
                        const HistoryCheckpointCallback &on_history_checkpoint = {});

        // Clear conversation history
        void clear_history();

        void set_thinking_budget(int budget) {
            thinking_budget_ = budget;
        }

        void set_environment_info(prompt::EnvironmentInfo info) {
            env_info_ = std::move(info);
        }

        void set_incoming_message_fetcher(IncomingMessageFetcher fetcher) {
            incoming_message_fetcher_ = std::move(fetcher);
        }

        void set_stop_requested_callback(StopRequestedCallback callback) {
            stop_requested_callback_ = std::move(callback);
        }

        // Replace conversation history (for session loading)
        void set_history(std::vector<Message> messages) {
            history_ = std::move(messages);
            tools_->clear_discovered();
        }

        // Access conversation history for persistence and export flows.
        [[nodiscard]]
        const std::vector<Message> &history() const {
            return history_;
        }

        [[nodiscard]]
        HistoryCompactionResult compress_history();

        [[nodiscard]]
        SessionMemoryDistillationResult distill_session_memory();

    private:
        ProviderSystem *provider_ = nullptr;
        ProviderRoute provider_route_;
        ToolRegistry *tools_ = nullptr;
        std::vector<Message> history_;
        memory::RuntimeMemory *memory_ = nullptr;
        std::string skills_prompt_;
        hooks::HookManager *hook_manager_ = nullptr;
        skills::SkillLoader *skill_loader_ = nullptr;
        int thinking_budget_ = 0;
        prompt::EnvironmentInfo env_info_;
        IncomingMessageFetcher incoming_message_fetcher_;
        StopRequestedCallback stop_requested_callback_;

        static constexpr int MAX_ITERATIONS = 20;

        bool inject_incoming_messages(const HistoryCheckpointCallback &on_history_checkpoint);
        [[nodiscard]]
        bool stop_requested() const;
    };

} // namespace orangutan::agent

namespace orangutan {

    using agent::AgentLoop;

} // namespace orangutan
