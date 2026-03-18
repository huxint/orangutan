#pragma once

#include "features/channel/core/channel.hpp"
#include "infra/config/config.hpp"
#include "features/channel/core/jid-task-runner.hpp"
#include "features/memory/memory.hpp"
#include "features/channel/core/message-queue.hpp"
#include "infra/storage/session-store.hpp"
#include "features/subagent/subagent-manager.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan {

class HookManager;
class CronStore;
class HeartbeatScheduler;

} // namespace orangutan

namespace orangutan::app {

struct AgentRuntimeConfig {
    std::string agent_key;
    std::string provider_name;
    std::string api_key;
    std::string model;
    std::vector<std::string> fallback_models;
    std::string base_url;
    std::string system_prompt;
    std::string workspace_root;
    std::string cli_runtime_key;
    std::string cli_memory_scope;
    Config::MemoryConfig memory;
    ToolPermissionSettings permissions;
    std::vector<std::string> allowed_child_agents;
};

class ChannelApprovalCoordinator {
public:
    explicit ChannelApprovalCoordinator(std::chrono::milliseconds timeout = std::chrono::minutes(2));

    [[nodiscard]]
    ToolApprovalCallback make_callback(const InboundMessage &message, ChannelManager &channel_manager);

    [[nodiscard]]
    bool handle_inbound_message(const InboundMessage &message, ChannelManager &channel_manager);

private:
    struct PendingApproval {
        std::string request_id;
        std::string jid;
        std::mutex mutex;
        std::condition_variable cv;
        bool resolved = false;
        bool approved = false;
    };

    std::chrono::milliseconds timeout_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_by_request_id_;
    std::unordered_map<std::string, std::vector<std::string>> pending_request_ids_by_jid_;
    std::uint64_t next_prompt_id_ = 0;

    void clear_pending(const std::shared_ptr<PendingApproval> &pending);
};

[[nodiscard]]
size_t default_serve_worker_count();

[[nodiscard]]
std::string resolve_agent_key_for_message(const InboundMessage &message, const std::unordered_map<std::string, std::string> &qq_bot_agents);

[[nodiscard]]
std::string resolve_reply_target(const InboundMessage &message);

void deliver_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager);

void deliver_command_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager);

[[nodiscard]]
std::string build_skill_prompt_for_runtime(const Config &cfg, const AgentRuntimeConfig &runtime_cfg);

void add_configured_channels(ChannelManager &channel_manager, const Config &cfg);

void run_channel_loop(MessageQueue &queue, ChannelManager &channel_manager, std::atomic<bool> &stop_requested, JidTaskRunner &task_runner,
                      const std::unordered_map<std::string, AgentRuntimeConfig> &agent_configs, const std::unordered_map<std::string, std::string> &qq_bot_agents,
                      MemoryStore *memory_store, SessionStore &session_store, SubagentManager &subagent_manager, const Config &cfg, orangutan::HookManager *hook_manager = nullptr,
                      CronStore *cron_store = nullptr, HeartbeatScheduler *heartbeat_scheduler = nullptr);

} // namespace orangutan::app
