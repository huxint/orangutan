#include "coordinator/coordinator-manager.hpp"
#include "coordinator/agent-definition-registry.hpp"
#include "swarm/mailbox.hpp"

#include <chrono>
#include <spdlog/spdlog.h>
#include <string>

namespace orangutan::coordinator {

    namespace {

        std::int64_t now_millis() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string format_task_notification(const AgentRunRecord &record) {
            std::string status_str;
            switch (record.status) {
                case AgentRunStatus::succeeded:
                    status_str = "completed";
                    break;
                case AgentRunStatus::failed:
                    status_str = "failed";
                    break;
                case AgentRunStatus::terminated:
                    status_str = "terminated";
                    break;
                case AgentRunStatus::abandoned:
                    status_str = "abandoned";
                    break;
                default:
                    status_str = "unknown";
                    break;
            }

            return "<task-notification>\n"
                   "  <task-id>" +
                   record.run_id +
                   "</task-id>\n"
                   "  <agent-key>" +
                   record.agent_key +
                   "</agent-key>\n"
                   "  <agent-name>" +
                   record.agent_name +
                   "</agent-name>\n"
                   "  <status>" +
                   status_str +
                   "</status>\n"
                   "  <summary>" +
                   record.task_summary +
                   "</summary>\n"
                   "  <result>" +
                   record.final_output +
                   "</result>\n"
                   "  <error>" +
                   record.error +
                   "</error>\n"
                   "</task-notification>";
        }

    } // namespace

    CoordinatorManager::CoordinatorManager(int max_concurrent_agents)
    : max_concurrent_(max_concurrent_agents) {}

    CoordinatorManager::~CoordinatorManager() {
        shutdown();
    }

    void CoordinatorManager::set_environment(AgentExecutionEnvironment env) {
        std::lock_guard lock(mutex_);
        env_ = env;
    }

    void CoordinatorManager::set_notification_callback(TaskNotificationCallback callback) {
        std::lock_guard lock(mutex_);
        notification_callback_ = std::move(callback);
    }

    std::string CoordinatorManager::make_run_id() {
        auto seq = next_run_id_++;
        return "run-" + std::to_string(now_millis()) + "-" + std::to_string(seq);
    }

    AgentSpawnResult CoordinatorManager::spawn(const AgentSpawnRequest &request) {
        std::lock_guard lock(mutex_);

        if (shutting_down_) {
            return AgentSpawnResult{.accepted = false, .error = "Coordinator is shutting down"};
        }

        // Check concurrent limit
        int active_count = 0;
        for (const auto &[id, run] : active_runs_) {
            std::lock_guard run_lock(run->mutex);
            if (!run->completed) {
                ++active_count;
            }
        }

        if (active_count >= max_concurrent_) {
            return AgentSpawnResult{.accepted = false, .error = "Maximum concurrent agent limit reached"};
        }

        // Validate agent key if registry available
        if (env_.definition_registry != nullptr && !env_.definition_registry->has(request.agent_key)) {
            return AgentSpawnResult{.accepted = false, .error = "Unknown agent key: " + request.agent_key};
        }

        auto run_id = make_run_id();
        auto agent_name = request.agent_name.empty() ? request.agent_key + "-" + std::to_string(next_run_id_) : request.agent_name;

        auto active_run = std::make_shared<ActiveRun>();
        active_run->record = AgentRunRecord{
            .run_id = run_id,
            .agent_key = request.agent_key,
            .agent_name = agent_name,
            .team_id = request.team_id,
            .parent_runtime_key = request.parent_runtime_key,
            .status = AgentRunStatus::queued,
            .task_summary = request.task_prompt,
            .started_at = now_millis(),
        };

        auto run_ptr = active_run;
        active_run->worker_thread = std::jthread([this, run_ptr](std::stop_token token) {
            run_worker(run_ptr, std::move(token));
        });

        active_runs_.emplace(run_id, std::move(active_run));

        spdlog::info("Spawned agent run: id={} key={} name={}", run_id, request.agent_key, agent_name);

        return AgentSpawnResult{
            .accepted = true,
            .run_id = run_id,
            .agent_name = agent_name,
        };
    }

    void CoordinatorManager::run_worker(const std::shared_ptr<ActiveRun> &run, std::stop_token stop_token) {
        {
            std::lock_guard lock(run->mutex);
            run->record.status = AgentRunStatus::running;
        }

        spdlog::info("Agent worker started: id={} key={}", run->record.run_id, run->record.agent_key);

        // Stub implementation: real AgentLoop integration comes later.
        // For now, just mark as succeeded with a placeholder output.

        // Check for cooperative cancellation
        if (stop_token.stop_requested()) {
            std::lock_guard lock(run->mutex);
            run->record.status = AgentRunStatus::terminated;
            run->record.completed_at = now_millis();
            run->completed = true;
            run->cv.notify_all();
            spdlog::info("Agent worker terminated before execution: id={}", run->record.run_id);
            return;
        }

        // Stub: complete immediately
        {
            std::lock_guard lock(run->mutex);
            run->record.status = AgentRunStatus::succeeded;
            run->record.final_output = "Agent task completed (stub implementation)";
            run->record.completed_at = now_millis();
            run->completed = true;
            run->cv.notify_all();
        }

        spdlog::info("Agent worker completed: id={}", run->record.run_id);

        // Notify coordinator
        TaskNotificationCallback callback;
        {
            std::lock_guard lock(mutex_);
            callback = notification_callback_;
        }

        if (callback) {
            std::lock_guard lock(run->mutex);
            callback(run->record);
        }

        spdlog::debug("Task notification: {}", format_task_notification(run->record));
    }

    void CoordinatorManager::send_message(const std::string &run_id, const std::string &from, const std::string &text) {
        std::lock_guard lock(mutex_);

        auto it = active_runs_.find(run_id);
        if (it == active_runs_.end()) {
            spdlog::warn("send_message: unknown run_id={}", run_id);
            return;
        }

        // If mailbox is available, route via mailbox using the agent's team and name
        if (env_.mailbox != nullptr) {
            std::lock_guard run_lock(it->second->mutex);
            auto &record = it->second->record;
            if (!record.team_id.empty()) {
                env_.mailbox->send(record.team_id, from, record.agent_name, text);
            }
        }

        spdlog::debug("Message sent to run_id={} from={}", run_id, from);
    }

    void CoordinatorManager::stop(const std::string &run_id) {
        std::shared_ptr<ActiveRun> run;
        {
            std::lock_guard lock(mutex_);
            auto it = active_runs_.find(run_id);
            if (it == active_runs_.end()) {
                spdlog::warn("stop: unknown run_id={}", run_id);
                return;
            }
            run = it->second;
        }

        // Request cooperative stop
        run->stop_source.request_stop();

        // Wait briefly for completion
        {
            std::unique_lock lock(run->mutex);
            run->cv.wait_for(lock, std::chrono::seconds(5), [&run] {
                return run->completed;
            });
        }

        // Force-mark as terminated if still not done
        {
            std::lock_guard lock(run->mutex);
            if (!run->completed) {
                run->record.status = AgentRunStatus::terminated;
                run->record.completed_at = now_millis();
                run->completed = true;
                run->cv.notify_all();
            }
        }

        spdlog::info("Stopped agent run: id={}", run_id);
    }

    std::optional<AgentRunRecord> CoordinatorManager::get_run(const std::string &run_id) const {
        std::lock_guard lock(mutex_);
        auto it = active_runs_.find(run_id);
        if (it == active_runs_.end()) {
            return std::nullopt;
        }
        std::lock_guard run_lock(it->second->mutex);
        return it->second->record;
    }

    std::vector<AgentRunRecord> CoordinatorManager::list_active_runs() const {
        std::lock_guard lock(mutex_);
        std::vector<AgentRunRecord> result;
        for (const auto &[id, run] : active_runs_) {
            std::lock_guard run_lock(run->mutex);
            if (run->record.status == AgentRunStatus::queued || run->record.status == AgentRunStatus::running) {
                result.push_back(run->record);
            }
        }
        return result;
    }

    void CoordinatorManager::shutdown() {
        std::vector<std::shared_ptr<ActiveRun>> runs_to_stop;
        {
            std::lock_guard lock(mutex_);
            if (shutting_down_) {
                return;
            }
            shutting_down_ = true;
            for (auto &[id, run] : active_runs_) {
                runs_to_stop.push_back(run);
            }
        }

        for (auto &run : runs_to_stop) {
            run->stop_source.request_stop();
        }

        // Wait for all to complete
        for (auto &run : runs_to_stop) {
            std::unique_lock lock(run->mutex);
            run->cv.wait_for(lock, std::chrono::seconds(10), [&run] {
                return run->completed;
            });
            if (!run->completed) {
                run->record.status = AgentRunStatus::abandoned;
                run->record.completed_at = now_millis();
                run->completed = true;
            }
        }

        // Join all threads (jthread destructor handles this, but we clear the map)
        {
            std::lock_guard lock(mutex_);
            active_runs_.clear();
        }

        spdlog::info("CoordinatorManager shutdown complete");
    }

} // namespace orangutan::coordinator
