#pragma once

#include "prompt/system-prompt-sections.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool-registry.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

    class AgentLoopBuilder;

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

        /// Create a builder for fluent AgentLoop configuration.
        [[nodiscard]]
        static AgentLoopBuilder configure(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools);

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

    class AgentLoopBuilder {
    public:
        AgentLoopBuilder(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools)
        : provider_(&provider),
          route_(std::move(route)),
          tools_(&tools) {}

        auto with_memory(this auto &&self, memory::RuntimeMemory *memory) -> decltype(auto) {
            self.memory_ = memory;
            return std::forward<decltype(self)>(self);
        }

        auto with_skills_prompt(this auto &&self, std::string prompt) -> decltype(auto) {
            self.skills_prompt_ = std::move(prompt);
            return std::forward<decltype(self)>(self);
        }

        auto with_hook_manager(this auto &&self, hooks::HookManager *manager) -> decltype(auto) {
            self.hook_manager_ = manager;
            return std::forward<decltype(self)>(self);
        }

        auto with_skill_loader(this auto &&self, skills::SkillLoader *loader) -> decltype(auto) {
            self.skill_loader_ = loader;
            return std::forward<decltype(self)>(self);
        }

        auto with_thinking_budget(this auto &&self, int budget) -> decltype(auto) {
            self.thinking_budget_ = budget;
            return std::forward<decltype(self)>(self);
        }

        auto with_environment_info(this auto &&self, prompt::EnvironmentInfo info) -> decltype(auto) {
            self.env_info_ = std::move(info);
            return std::forward<decltype(self)>(self);
        }

        auto with_incoming_message_fetcher(this auto &&self, AgentLoop::IncomingMessageFetcher fetcher) -> decltype(auto) {
            self.incoming_message_fetcher_ = std::move(fetcher);
            return std::forward<decltype(self)>(self);
        }

        auto with_stop_requested_callback(this auto &&self, AgentLoop::StopRequestedCallback callback) -> decltype(auto) {
            self.stop_requested_callback_ = std::move(callback);
            return std::forward<decltype(self)>(self);
        }

        auto with_history(this auto &&self, std::vector<Message> messages) -> decltype(auto) {
            self.history_ = std::move(messages);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> std::expected<AgentLoop, std::string> {
            if (provider_ == nullptr) { return std::unexpected("provider is required"); }
            if (tools_ == nullptr) { return std::unexpected("tool registry is required"); }
            AgentLoop loop(*provider_, route_, *tools_, memory_, skills_prompt_, hook_manager_, skill_loader_);
            if (thinking_budget_ > 0) { loop.set_thinking_budget(thinking_budget_); }
            if (env_info_.has_value()) { loop.set_environment_info(*env_info_); }
            if (incoming_message_fetcher_) { loop.set_incoming_message_fetcher(incoming_message_fetcher_); }
            if (stop_requested_callback_) { loop.set_stop_requested_callback(stop_requested_callback_); }
            if (!history_.empty()) { loop.set_history(std::move(history_)); }
            return loop;
        }

    private:
        ProviderSystem *provider_ = nullptr;
        ProviderRoute route_;
        ToolRegistry *tools_ = nullptr;
        memory::RuntimeMemory *memory_ = nullptr;
        std::string skills_prompt_;
        hooks::HookManager *hook_manager_ = nullptr;
        skills::SkillLoader *skill_loader_ = nullptr;
        int thinking_budget_ = 0;
        std::optional<prompt::EnvironmentInfo> env_info_;
        AgentLoop::IncomingMessageFetcher incoming_message_fetcher_;
        AgentLoop::StopRequestedCallback stop_requested_callback_;
        mutable std::vector<Message> history_;
    };

} // namespace orangutan::agent

namespace orangutan {

    using agent::AgentLoop;

} // namespace orangutan
