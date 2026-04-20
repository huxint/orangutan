#pragma once

#include "types/types.hpp"
#include "bootstrap/channel-serve-delivery.hpp"
#include "bootstrap/channel-serve-runtime.hpp"
#include "channel/channel.hpp"
#include "config/config.hpp"
#include "permissions/permission-types.hpp"
#include "providers/provider.hpp"
#include "channel/jid-task-runner.hpp"
#include "memory/memory-store.hpp"
#include "channel/message-queue.hpp"
#include "storage/session-store.hpp"
#include "tools/registry/tool.hpp"
#include "tools/registry/tool-context.hpp"
#include "utils/task-pool.hpp"
#include "utils/transparent-lookup.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace orangutan::agent {
    class AgentLoop;
}

namespace orangutan::automation {
    class AutomationRuntime;
}

namespace orangutan::hooks {
    class HookManager;
}

namespace orangutan::orchestration {
    class OrchestrationManager;
    class AgentMailbox;
    class TeamManager;
} // namespace orangutan::orchestration

namespace orangutan::bootstrap {

    struct AgentRuntimeBundle;

    class ChannelApprovalCoordinator {
    public:
        explicit ChannelApprovalCoordinator(std::chrono::milliseconds timeout = std::chrono::minutes(2));

        [[nodiscard]]
        ApprovalCallback make_callback(const InboundMessage &message, ChannelManager &channel_manager, JidTaskRunner *task_runner = nullptr,
                                       tools::PermissionRuleMutationCallback permission_rule_mutator = {});

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
            bool always_allow = false;
            bool allow_always_eligible = false;
            bool allow_text_reply = true;
            bool cancelled = false;
        };

        std::chrono::milliseconds timeout_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_by_request_id_;
        std::unordered_map<std::string, std::vector<std::string>> pending_request_ids_by_jid_;
        std::unordered_set<std::string> qq_keyboard_disabled_keys_;
        base::u64 next_prompt_id_ = 0;
        bool shutting_down_ = false;

        void clear_pending(const std::shared_ptr<PendingApproval> &pending);
    };

    [[nodiscard]]
    std::size_t default_serve_worker_count();

    [[nodiscard]]
    std::string resolve_agent_key_for_message(const InboundMessage &message, const utils::transparent_string_unordered_map<std::string> &qq_bot_agents);

    [[nodiscard]]
    std::string build_skill_prompt_for_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg);

    void add_configured_channels(ChannelManager &channel_manager, const Config &cfg, utils::TaskPool &task_pool);

    void run_channel_loop(MessageQueue &queue, ChannelManager &channel_manager, std::atomic<bool> &stop_requested, JidTaskRunner &task_runner,
                          const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const utils::transparent_string_unordered_map<std::string> &qq_bot_agents,
                          MemoryStore *memory_store, SessionStore &session_store, orchestration::OrchestrationManager *orchestration_manager, const Config &cfg,
                          hooks::HookManager *hook_manager = nullptr, automation::AutomationRuntime *automation_runtime = nullptr, orchestration::TeamManager *team_manager = nullptr,
                          orchestration::AgentMailbox *mailbox = nullptr);

} // namespace orangutan::bootstrap
