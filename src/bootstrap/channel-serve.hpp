#pragma once

#include "types/types.hpp"
#include "channel/channel.hpp"
#include "config/config.hpp"
#include "providers/provider.hpp"
#include "channel/jid-task-runner.hpp"
#include "memory/memory-store.hpp"
#include "channel/message-queue.hpp"
#include "storage/session-store.hpp"
#include "tools/registry/tool.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::agent {
    class AgentLoop;
}

namespace orangutan::automation {
    class Runtime;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::coordinator {
    class CoordinatorManager;
}

namespace orangutan::providers {
    class Provider;
}

namespace orangutan::bootstrap {

    struct AgentRuntimeConfig {
        std::string agent_key;
        std::string model;
        std::vector<std::string> fallback_models;
        providers::ProviderEndpoint primary_endpoint;
        std::vector<providers::ProviderEndpoint> fallback_endpoints;
        std::string workspace_root;
        std::string edit_mode = "hashline";
        int thinking_budget = 0;
        std::string cli_runtime_key;
        std::string cli_memory_scope;
        Config::MemoryConfig memory;
        ToolPermissionSettings permissions;
        std::vector<std::string> team_agents;
        bool coordinator_mode = false;
        int max_concurrent_agents = 4;
    };

    struct ConversationRuntimeInspection {
        std::vector<ToolDef> tool_definitions;
        base::origin runtime_origin = base::origin::cli;
        std::string raw_caller_id;
        bool has_agent = false;
        bool has_hook_manager = false;
        std::string session_scope_key;
        std::string configured_model;
        std::vector<std::string> fallback_models;
    };

    class ChannelApprovalCoordinator {
    public:
        explicit ChannelApprovalCoordinator(std::chrono::milliseconds timeout = std::chrono::minutes(2));

        [[nodiscard]]
        ToolApprovalCallback make_callback(const InboundMessage &message, ChannelManager &channel_manager, JidTaskRunner *task_runner = nullptr);

        [[nodiscard]]
        bool handle_inbound_message(const InboundMessage &message, ChannelManager &channel_manager);

        void shutdown();

    private:
        struct PendingApproval {
            std::string request_id;
            std::string jid;
            std::mutex mutex;
            std::condition_variable cv;
            bool resolved = false;
            bool approved = false;
            bool cancelled = false;
        };

        std::chrono::milliseconds timeout_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_by_request_id_;
        std::unordered_map<std::string, std::vector<std::string>> pending_request_ids_by_jid_;
        base::u64 next_prompt_id_ = 0;
        bool shutting_down_ = false;

        void clear_pending(const std::shared_ptr<PendingApproval> &pending);
    };

    [[nodiscard]]
    std::size_t default_serve_worker_count();

    [[nodiscard]]
    std::string resolve_agent_key_for_message(const InboundMessage &message, const std::unordered_map<std::string, std::string> &qq_bot_agents);

    [[nodiscard]]
    std::string resolve_reply_target(const InboundMessage &message);

    void deliver_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager);

    void deliver_command_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager);

    [[nodiscard]]
    std::string build_skill_prompt_for_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg);

    namespace detail {

        struct ChannelCompletionResumeState {
            std::mutex mutex;
            agent::AgentLoop *agent = nullptr;
            providers::Provider *provider = nullptr;
            hooks::HookManager *hook_manager = nullptr;
            std::string *current_session_id = nullptr;
            std::size_t *persisted_message_count = nullptr;
            SessionStore *session_store = nullptr;
            ChannelManager *channel_manager = nullptr;
            std::string jid;
            std::string agent_key;
            std::string configured_model;
            std::string session_scope_key;
            automation::Runtime *automation_runtime = nullptr;
        };

        [[nodiscard]]
        ConversationRuntimeInspection inspect_conversation_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg, MemoryStore *memory_store,
                                                                   coordinator::CoordinatorManager *coordinator_manager, const std::string &raw_caller_id,
                                                                   hooks::HookManager *hook_manager = nullptr, automation::Runtime *automation_runtime = nullptr);

        [[nodiscard]]
        BackgroundCompletionResumeCallback make_channel_completion_resume_callback(const std::weak_ptr<ChannelCompletionResumeState> &state);

    } // namespace detail

    void add_configured_channels(ChannelManager &channel_manager, const Config &cfg);

    void run_channel_loop(MessageQueue &queue, ChannelManager &channel_manager, std::atomic<bool> &stop_requested, JidTaskRunner &task_runner,
                          const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const std::unordered_map<std::string, std::string> &qq_bot_agents,
                          MemoryStore *memory_store, SessionStore &session_store, coordinator::CoordinatorManager *coordinator_manager, const Config &cfg,
                          hooks::HookManager *hook_manager = nullptr, automation::Runtime *automation_runtime = nullptr);

} // namespace orangutan::bootstrap
