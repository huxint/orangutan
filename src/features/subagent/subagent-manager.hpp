#pragma once

#include "app/runtime/identity.hpp"
#include "core/types.hpp"
#include "infra/config/config.hpp"
#include "infra/storage/subagent-run-store.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orangutan {

class MemoryStore;
class Provider;
class SessionStore;

struct SubagentCallerContext {
    SubagentRuntimeOrigin runtime_origin = SubagentRuntimeOrigin::cli;
    std::string runtime_key;
    std::string agent_key;
    std::string scope_key;
    std::string raw_caller_id;
    std::optional<std::string> session_id;
    std::vector<std::string> allowed_child_agents;
    bool is_child_run = false;
    ToolApprovalCallback approval_callback;
};

struct SubagentSpawnRequest {
    SubagentCallerContext caller;
    std::string child_agent_key;
    std::string child_session_id;
    std::string child_scope_key;
    std::string task_summary;
};

struct SubagentSpawnResult {
    bool accepted = false;
    std::string run_id;
    std::string error;
};

struct SubagentWorkerRequest {
    std::string run_id;
    SubagentCallerContext caller;
    std::string child_agent_key;
    std::string child_scope_key;
    std::string child_session_id;
    std::string task_summary;
    RuntimeIdentity child_identity;
    std::stop_token stop_token;
};

struct SubagentChildRuntimeConfig {
    std::string agent_key;
    std::string provider_name;
    std::string api_key;
    std::string model;
    std::vector<std::string> fallback_models;
    std::string base_url;
    std::string system_prompt;
    std::string workspace_root;
    Config::MemoryConfig memory;
    ToolPermissionSettings permissions;
    std::vector<std::string> allowed_child_agents;
};

struct SubagentExecutionEnvironment {
    const std::unordered_map<std::string, SubagentChildRuntimeConfig> *agent_configs = nullptr;
    SessionStore *session_store = nullptr;
    MemoryStore *memory_store = nullptr;
    std::function<std::unique_ptr<Provider>(const SubagentChildRuntimeConfig &config)> provider_factory;
};

struct SubagentWorkerResult {
    SubagentRunStatus status = SubagentRunStatus::succeeded;
    std::string final_summary;
    std::string final_output;
    std::string error;
};

enum class SubagentWaitState {
    completed,
    timed_out,
    not_found,
};

struct SubagentWaitResult {
    SubagentWaitState state = SubagentWaitState::not_found;
    std::optional<SubagentRunRecord> run;
};

struct SubagentStatusRequest {
    std::string run_id;
    SubagentCallerContext caller;
};

struct SubagentStatusResult {
    std::optional<SubagentRunRecord> run;
};

struct SubagentWaitRequest {
    std::string run_id;
    std::chrono::milliseconds timeout{0};
    SubagentCallerContext caller;
};

class SubagentManager {
public:
    using Worker = std::function<SubagentWorkerResult(const SubagentWorkerRequest &request)>;

    SubagentManager(SubagentRunStore &run_store, Worker worker);
    SubagentManager(SubagentRunStore &run_store, SubagentExecutionEnvironment environment);
    ~SubagentManager();

    SubagentManager(const SubagentManager &) = delete;
    SubagentManager &operator=(const SubagentManager &) = delete;
    SubagentManager(SubagentManager &&) = delete;
    SubagentManager &operator=(SubagentManager &&) = delete;

    [[nodiscard]]
    SubagentSpawnResult spawn(const SubagentSpawnRequest &request);

    [[nodiscard]]
    SubagentStatusResult status(const SubagentStatusRequest &request);

    [[nodiscard]]
    SubagentWaitResult wait(const SubagentWaitRequest &request);

    void abandon_stale_runs(const std::string &parent_runtime_key);

    void shutdown();

private:
    struct ActiveRunState {
        mutable std::mutex mutex;
        std::condition_variable cv;
        bool completed = false;
        bool abandoned = false;
        bool worker_exited = false;
        std::stop_source stop_source;
        std::thread worker_thread;
    };

    SubagentRunStore &run_store_;
    Worker worker_;
    const std::unordered_map<std::string, SubagentChildRuntimeConfig> *agent_configs_ = nullptr;
    SessionStore *session_store_ = nullptr;
    MemoryStore *memory_store_ = nullptr;
    std::function<std::unique_ptr<Provider>(const SubagentChildRuntimeConfig &config)> provider_factory_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ActiveRunState>> active_runs_;
    bool shutting_down_ = false;
    std::uint64_t next_run_id_ = 0;

    [[nodiscard]]
    std::string make_run_id();

    static bool is_terminal_status(SubagentRunStatus status);
    static bool is_allowed_child_agent(const SubagentCallerContext &caller, const std::string &child_agent_key);
    static bool can_access_run(const SubagentCallerContext &caller, const SubagentRunRecord &run);
    [[nodiscard]]
    bool uses_real_execution() const;
    [[nodiscard]]
    std::optional<SubagentChildRuntimeConfig> resolve_child_config(const std::string &child_agent_key) const;
    [[nodiscard]]
    SubagentWorkerResult run_real_child(const SubagentWorkerRequest &request);

    void run_worker(const std::shared_ptr<ActiveRunState> &state, const SubagentWorkerRequest &request);
    static void finish_state(const std::shared_ptr<ActiveRunState> &state);
    [[nodiscard]]
    static bool should_abandon(const std::shared_ptr<ActiveRunState> &state);
    static bool is_finished(const std::shared_ptr<ActiveRunState> &state);
    void cleanup_finished_run(const std::string &run_id, const std::shared_ptr<ActiveRunState> &state);
};

} // namespace orangutan
