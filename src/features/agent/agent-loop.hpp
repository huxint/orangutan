#pragma once

#include "core/providers/provider.hpp"
#include "core/tools/tool.hpp"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan {

    class HookManager;
    class RuntimeMemory;

    class AgentLoop {
    public:
        using ToolEventCallback = std::function<void(const std::string &event_type, const ToolUse &call, const ToolResult *result)>;
        using HistoryCheckpointCallback = std::function<void(const std::vector<Message> &history)>;
        struct HistoryCompactionResult {
            bool compacted = false;
            size_t messages_before = 0;
            size_t messages_after = 0;
            std::string status;
        };

        struct SessionMemoryDistillationResult {
            bool distilled = false;
            size_t memories_stored = 0;
            bool journal_stored = false;
            std::string status;
        };

        AgentLoop(Provider &provider, ToolRegistry &tools, const std::string &system_prompt = "", RuntimeMemory *memory = nullptr, std::string skills_prompt = {},
                  HookManager *hook_manager = nullptr);

        // Process one user message: run the ReAct loop until final text response
        std::string run(const std::string &user_input, const StreamCallback &on_stream_event = {}, const ToolEventCallback &on_tool_event = {},
                        const HistoryCheckpointCallback &on_history_checkpoint = {});

        // Clear conversation history
        void clear_history();

        void set_thinking_budget(int budget) {
            thinking_budget_ = budget;
        }

        // Replace conversation history (for session loading)
        void set_history(std::vector<Message> messages) {
            history_ = std::move(messages);
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
        std::string system_prompt_;
        RuntimeMemory *memory_ = nullptr;
        std::string skills_prompt_;
        HookManager *hook_manager_ = nullptr;
        int thinking_budget_ = 0;

        static constexpr int max_iterations = 20;
        static constexpr int max_continuations = 3;
        static constexpr int loop_detection_threshold = 3;
        static constexpr int compaction_threshold = 50;
        static constexpr int compaction_keep_recent = 10;
        static constexpr size_t max_memory_prompt_bytes = 2048;

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

        // Returns true if loop detected (and injects correction message)
        bool check_loop_detection(const ToolUse &call);

        // Execute tools, check for loops, return (result_blocks, loop_detected)
        std::pair<std::vector<Content>, bool> execute_tools(const std::vector<ToolUse> &calls, bool human_output, const ToolEventCallback &on_tool_event);

        // Handle max_tokens continuation (returns appended text)
        std::string handle_continuation(const std::string &system_prompt, bool &first_text, bool human_output, const StreamCallback &on_stream_event,
                                        const ToolEventCallback &on_tool_event, const HistoryCheckpointCallback &on_history_checkpoint);

        // Compact history if it exceeds the threshold
        [[nodiscard]]
        HistoryCompactionResult compact_history(size_t minimum_history_size);

        [[nodiscard]]
        std::string build_system_prompt(const std::string &user_input) const;

        [[nodiscard]]
        std::string build_session_memory_transcript() const;
    };

} // namespace orangutan
