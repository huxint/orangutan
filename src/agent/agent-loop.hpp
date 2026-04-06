#pragma once

#include "prompt/system-prompt-sections.hpp"
#include "providers/provider.hpp"
#include "tools/registry/tool.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include "types/base.hpp"

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::memory {
    class RuntimeMemory;
}

namespace orangutan::agent {

    class AgentLoop {
    public:
        using ToolEventCallback = std::function<void(const std::string &event_type, const ToolUse &call, const ToolResult *result)>;
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

        AgentLoop(Provider &provider, ToolRegistry &tools, memory::RuntimeMemory *memory = nullptr, std::string skills_prompt = {}, hooks::HookManager *hook_manager = nullptr);

        // Process one user message: run the ReAct loop until final text response
        std::string run(const std::string &user_input, const StreamCallback &on_stream_event = {}, const ToolEventCallback &on_tool_event = {},
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
            tools_.clear_discovered();
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
        Provider &provider_;
        ToolRegistry &tools_;
        std::vector<Message> history_;
        memory::RuntimeMemory *memory_ = nullptr;
        std::string skills_prompt_;
        hooks::HookManager *hook_manager_ = nullptr;
        int thinking_budget_ = 0;
        prompt::EnvironmentInfo env_info_;
        IncomingMessageFetcher incoming_message_fetcher_;
        StopRequestedCallback stop_requested_callback_;

        static constexpr int MAX_ITERATIONS = 20;
        static constexpr int MAX_CONTINUATIONS = 3;
        static constexpr int LOOP_DETECTION_THRESHOLD = 3;
        static constexpr int LOOP_ABORT_THRESHOLD = 5;
        static constexpr int COMPACTION_THRESHOLD = 50;
        static constexpr int COMPACTION_KEEP_RECENT = 10;
        static constexpr std::size_t MAX_MEMORY_PROMPT_BYTES = 4096;

        // Loop detection: tracks (tool_name, input_hash) call counts per run
        struct ToolCallSignature {
            std::string name;
            std::size_t input_hash;

            bool operator==(const ToolCallSignature &other) const = default;
        };

        struct SignatureHash {
            std::size_t operator()(const ToolCallSignature &sig) const {
                auto h1 = std::hash<std::string>{}(sig.name);
                auto h2 = sig.input_hash;
                return h1 ^ (h2 << 1);
            }
        };

        std::unordered_map<ToolCallSignature, int, SignatureHash> call_counts_;

        enum class loop_status : base::u8 {
            ok,
            warning,
            abort
        };

        // Returns loop status for the given tool call
        loop_status check_loop_detection(const ToolUse &call);

        // Execute tools, check for loops, return (result_blocks, loop_status)
        std::pair<std::vector<Content>, loop_status> execute_tools(const std::vector<ToolUse> &calls, bool human_output, const ToolEventCallback &on_tool_event);

        // Handle max_tokens continuation (returns appended text)
        std::string handle_continuation(const std::string &system_prompt, bool &first_text, bool human_output, const StreamCallback &on_stream_event,
                                        const ToolEventCallback &on_tool_event, const HistoryCheckpointCallback &on_history_checkpoint);

        // Compact history if it exceeds the threshold
        [[nodiscard]]
        HistoryCompactionResult compact_history(std::size_t minimum_history_size);

        [[nodiscard]]
        std::string build_system_prompt(const std::string &user_input) const;

        [[nodiscard]]
        std::string build_session_memory_transcript() const;

        bool inject_incoming_messages(const HistoryCheckpointCallback &on_history_checkpoint);
        [[nodiscard]]
        bool stop_requested() const;
    };

} // namespace orangutan::agent

namespace orangutan {

    using agent::AgentLoop;

} // namespace orangutan
