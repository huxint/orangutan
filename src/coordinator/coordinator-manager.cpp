#include "coordinator/coordinator-manager.hpp"
#include "coordinator/agent-definition-registry.hpp"
#include "swarm/mailbox.hpp"
#include "utils/escape.hpp"
#include "utils/task-pool.hpp"

#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace orangutan::coordinator {

    namespace {

        std::int64_t now_millis() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string format_task_notification(const AgentRunRecord &record) {
            std::string status_str;
            switch (record.status) {
                case agent_run_status::succeeded:
                    status_str = "completed";
                    break;
                case agent_run_status::failed:
                    status_str = "failed";
                    break;
                case agent_run_status::terminated:
                    status_str = "terminated";
                    break;
                case agent_run_status::abandoned:
                    status_str = "abandoned";
                    break;
                default:
                    status_str = "unknown";
                    break;
            }

            return "<task-notification>\n"
                   "  <task-id>" +
                   utils::escape_xml(record.run_id) +
                   "</task-id>\n"
                   "  <agent-key>" +
                   utils::escape_xml(record.agent_key) +
                   "</agent-key>\n"
                   "  <agent-name>" +
                   utils::escape_xml(record.agent_name) +
                   "</agent-name>\n"
                   "  <status>" +
                   utils::escape_xml(status_str) +
                   "</status>\n"
                   "  <summary>" +
                   utils::escape_xml(record.task_summary) +
                   "</summary>\n"
                   "  <result>" +
                   utils::escape_xml(record.final_output) +
                   "</result>\n"
                   "  <error>" +
                   utils::escape_xml(record.error) +
                   "</error>\n"
                   "</task-notification>";
        }

    } // namespace

    struct CoordinatorManager::Impl {
        utils::TaskPool pool;
        exec::async_scope scope;

        explicit Impl(std::size_t pool_size)
        : pool{pool_size} {}
    };

    namespace {

        constexpr std::size_t MIN_POOL_SIZE = 2;

        [[nodiscard]]
        std::size_t resolve_pool_size(int max_concurrent) {
            return std::max<std::size_t>(static_cast<std::size_t>(std::max(1, max_concurrent)), MIN_POOL_SIZE);
        }

    } // namespace

    CoordinatorManager::CoordinatorManager(int max_concurrent_agents)
    : max_concurrent_(std::max(1, max_concurrent_agents)),
      impl_(std::make_unique<Impl>(resolve_pool_size(max_concurrent_agents))) {}

    CoordinatorManager::~CoordinatorManager() {
        shutdown();
    }

    void CoordinatorManager::set_environment(AgentExecutionEnvironment env) {
        std::scoped_lock lock(mutex_);
        env_ = env;
    }

    void CoordinatorManager::set_notification_callback(TaskNotificationCallback callback) {
        std::scoped_lock lock(mutex_);
        notification_callback_ = std::move(callback);
    }

    void CoordinatorManager::register_runtime_notification_handler(std::string runtime_key, RuntimeNotificationHandler handler) {
        if (runtime_key.empty() || !handler) {
            return;
        }

        std::scoped_lock lock(mutex_);
        runtime_notification_handlers_.insert_or_assign(std::move(runtime_key), std::move(handler));
    }

    void CoordinatorManager::unregister_runtime_notification_handler(const std::string &runtime_key) {
        std::scoped_lock lock(mutex_);
        runtime_notification_handlers_.erase(runtime_key);
    }

    void CoordinatorManager::set_worker_runtime_factory(WorkerRuntimeFactory factory) {
        std::scoped_lock lock(mutex_);
        worker_runtime_factory_ = std::move(factory);
    }

    CoordinatorManagerBuilder CoordinatorManager::configure(CoordinatorManager &manager) {
        return CoordinatorManagerBuilder(manager);
    }

    std::string CoordinatorManager::make_run_id() {
        auto seq = next_run_id_++;
        return "run-" + std::to_string(now_millis()) + "-" + std::to_string(seq);
    }

    AgentSpawnResult CoordinatorManager::spawn(const AgentSpawnRequest &request) {
        std::scoped_lock lock(mutex_);

        if (shutting_down_) {
            return AgentSpawnResult{.accepted = false, .error = "Coordinator is shutting down"};
        }

        // Validate agent key if registry available
        if (env_.definition_registry != nullptr && !env_.definition_registry->has(request.agent_key)) {
            return AgentSpawnResult{.accepted = false, .error = "Unknown agent key: " + request.agent_key};
        }

        auto run_id = make_run_id();
        auto agent_name = request.agent_name.empty() ? request.agent_key + "-" + std::to_string(next_run_id_) : request.agent_name;

        auto active_run = std::make_shared<ActiveRun>();
        active_run->request = request;
        active_run->record = AgentRunRecord{
            .run_id = run_id,
            .agent_key = request.agent_key,
            .agent_name = agent_name,
            .team_id = request.team_id,
            .parent_runtime_key = request.parent_runtime_key,
            .status = agent_run_status::queued,
            .task_summary = request.task_prompt,
            .started_at = now_millis(),
        };

        active_runs_.try_emplace(run_id, std::move(active_run));

        auto run_it = active_runs_.find(run_id);
        if (count_running_locked() < max_concurrent_) {
            launch_run_locked(run_it->second);
        } else {
            pending_run_ids_.push_back(run_id);
        }

        spdlog::info("spawned agent run: id={} key={} name={}", run_id, request.agent_key, agent_name);

        return AgentSpawnResult{
            .accepted = true,
            .run_id = run_id,
            .agent_name = agent_name,
        };
    }

    int CoordinatorManager::count_running_locked() const {
        int running_count = 0;
        for (const auto &[id, run] : active_runs_) {
            std::scoped_lock run_lock(run->mutex);
            if (run->record.status == agent_run_status::running && !run->completed) {
                ++running_count;
            }
        }
        return running_count;
    }

    void CoordinatorManager::launch_run_locked(const std::shared_ptr<ActiveRun> &run) {
        {
            std::scoped_lock run_lock(run->mutex);
            run->record.status = agent_run_status::running;
        }
        auto sender = stdexec::schedule(impl_->pool.scheduler())
                    | stdexec::then([this, run] {
                          run_worker(run, run->stop_source.get_token());
                      });
        impl_->scope.spawn(std::move(sender));
    }

    void CoordinatorManager::remove_pending_run_locked(const std::string &run_id) {
        std::erase(pending_run_ids_, run_id);
    }

    void CoordinatorManager::maybe_start_queued_runs() {
        std::vector<std::shared_ptr<ActiveRun>> runs_to_launch;
        {
            std::scoped_lock lock(mutex_);
            while (!pending_run_ids_.empty() && count_running_locked() + static_cast<int>(runs_to_launch.size()) < max_concurrent_) {
                auto run_id = pending_run_ids_.front();
                pending_run_ids_.pop_front();

                const auto it = active_runs_.find(run_id);
                if (it == active_runs_.end()) {
                    continue;
                }

                std::scoped_lock run_lock(it->second->mutex);
                if (it->second->completed || it->second->record.status != agent_run_status::queued) {
                    continue;
                }

                runs_to_launch.push_back(it->second);
            }

            for (const auto &run : runs_to_launch) {
                launch_run_locked(run);
            }
        }
    }

    void CoordinatorManager::run_worker(const std::shared_ptr<ActiveRun> &run, const std::stop_token &stop_token) {
        const auto run_id = run->record.run_id;

        {
            std::scoped_lock lock(run->mutex);
            run->record.status = agent_run_status::running;
        }

        spdlog::info("agent worker started: id={} key={}", run_id, run->record.agent_key);

        if (stop_token.stop_requested()) {
            std::scoped_lock lock(run->mutex);
            run->record.status = agent_run_status::terminated;
            run->record.completed_at = now_millis();
            run->completed = true;
            run->cv.notify_all();
            spdlog::info("agent worker terminated before execution: id={}", run_id);
            return;
        }

        WorkerRuntimeFactory factory;
        {
            std::scoped_lock lock(mutex_);
            factory = worker_runtime_factory_;
        }

        if (!factory) {
            std::scoped_lock lock(run->mutex);
            run->record.status = agent_run_status::failed;
            run->record.error = "No worker runtime factory configured";
            run->record.completed_at = now_millis();
            run->completed = true;
            run->cv.notify_all();
            spdlog::warn("agent worker failed (no factory): id={}", run_id);
        } else {
            try {
                auto worker = factory(run->request);
                auto output = worker->run(run->request.task_prompt, stop_token);

                std::scoped_lock lock(run->mutex);
                run->record.status = stop_token.stop_requested() ? agent_run_status::terminated : agent_run_status::succeeded;
                run->record.final_output = std::move(output);
                run->record.completed_at = now_millis();
                run->completed = true;
                run->cv.notify_all();
            } catch (const std::exception &e) {
                std::scoped_lock lock(run->mutex);
                run->record.status = agent_run_status::failed;
                run->record.error = e.what();
                run->record.completed_at = now_millis();
                run->completed = true;
                run->cv.notify_all();
            }
        }

        spdlog::info("agent worker completed: id={}", run_id);

        TaskNotificationCallback callback;
        RuntimeNotificationHandler runtime_handler;
        std::string notification_xml;
        {
            std::scoped_lock lock(mutex_);
            callback = notification_callback_;
            if (!run->record.parent_runtime_key.empty()) {
                const auto it = runtime_notification_handlers_.find(run->record.parent_runtime_key);
                if (it != runtime_notification_handlers_.end()) {
                    runtime_handler = it->second;
                }
            }
        }

        {
            std::scoped_lock lock(run->mutex);
            notification_xml = format_task_notification(run->record);
        }

        if (runtime_handler) {
            if (const auto error = runtime_handler(notification_xml); error.has_value()) {
                spdlog::warn("failed to resume runtime {} with task notification: {}", run->record.parent_runtime_key, *error);
            }
        }

        if (callback) {
            std::scoped_lock lock(run->mutex);
            callback(run->record);
        }

        spdlog::debug("task notification: {}", notification_xml);
        maybe_start_queued_runs();
    }

    std::optional<std::string> CoordinatorManager::send_message(const std::string &run_id, const std::string &from, const std::string &text) {
        std::scoped_lock lock(mutex_);

        auto it = active_runs_.find(run_id);
        if (it == active_runs_.end()) {
            spdlog::warn("send_message: unknown run_id={}", run_id);
            return "Unknown run_id: " + run_id;
        }

        if (env_.mailbox == nullptr) {
            return "Mailbox is not available";
        }

        std::scoped_lock run_lock(it->second->mutex);
        auto &record = it->second->record;
        if (record.team_id.empty()) {
            return "Target run is not attached to a team mailbox";
        }

        env_.mailbox->send(record.team_id, from, record.agent_name, text);
        spdlog::debug("message sent to run_id={} from={}", run_id, from);
        return std::nullopt;
    }

    void CoordinatorManager::stop(const std::string &run_id) {
        std::shared_ptr<ActiveRun> run;
        bool stopped_queued_run = false;
        {
            std::scoped_lock lock(mutex_);
            auto it = active_runs_.find(run_id);
            if (it == active_runs_.end()) {
                spdlog::warn("stop: unknown run_id={}", run_id);
                return;
            }
            if (it->second->record.status == agent_run_status::queued) {
                remove_pending_run_locked(run_id);
                std::scoped_lock run_lock(it->second->mutex);
                it->second->record.status = agent_run_status::terminated;
                it->second->record.completed_at = now_millis();
                it->second->completed = true;
                it->second->cv.notify_all();
                stopped_queued_run = true;
            } else {
                run = it->second;
            }
        }

        if (stopped_queued_run) {
            spdlog::info("stopped queued agent run: id={}", run_id);
            maybe_start_queued_runs();
            return;
        }

        // Request cooperative stop on the worker's stop source.
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
            std::scoped_lock lock(run->mutex);
            if (!run->completed) {
                run->record.status = agent_run_status::terminated;
                run->record.completed_at = now_millis();
                run->completed = true;
                run->cv.notify_all();
            }
        }

        spdlog::info("stopped agent run: id={}", run_id);
        maybe_start_queued_runs();
    }

    std::optional<AgentRunRecord> CoordinatorManager::get_run(const std::string &run_id) const {
        std::scoped_lock lock(mutex_);
        auto it = active_runs_.find(run_id);
        if (it == active_runs_.end()) {
            return std::nullopt;
        }
        std::scoped_lock run_lock(it->second->mutex);
        return it->second->record;
    }

    std::vector<AgentRunRecord> CoordinatorManager::list_active_runs() const {
        std::scoped_lock lock(mutex_);
        std::vector<AgentRunRecord> result;
        for (const auto &[id, run] : active_runs_) {
            std::scoped_lock run_lock(run->mutex);
            if (run->record.status == agent_run_status::queued || run->record.status == agent_run_status::running) {
                result.push_back(run->record);
            }
        }
        return result;
    }

    void CoordinatorManager::shutdown() {
        std::vector<std::shared_ptr<ActiveRun>> runs_to_stop;
        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_) {
                return;
            }
            shutting_down_ = true;
            pending_run_ids_.clear();
            for (auto &[id, run] : active_runs_) {
                static_cast<void>(id);
                std::scoped_lock run_lock(run->mutex);
                if (!run->completed && run->record.status == agent_run_status::queued) {
                    run->record.status = agent_run_status::terminated;
                    run->record.completed_at = now_millis();
                    run->completed = true;
                    run->cv.notify_all();
                    continue;
                }
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
                run->record.status = agent_run_status::abandoned;
                run->record.completed_at = now_millis();
                run->completed = true;
            }
        }

        if (impl_ != nullptr) {
            static_cast<void>(stdexec::sync_wait(impl_->scope.on_empty()));
        }

        {
            std::scoped_lock lock(mutex_);
            active_runs_.clear();
        }

        spdlog::info("coordinatormanager shutdown complete");
    }

} // namespace orangutan::coordinator
