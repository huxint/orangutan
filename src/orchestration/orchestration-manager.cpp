#include "orchestration/orchestration-manager.hpp"

#include "orchestration/agent-definition-registry.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "utils/escape.hpp"
#include "utils/task-pool.hpp"
#include "utils/time-format.hpp"

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace orangutan::orchestration {

    namespace {

        constexpr std::size_t MIN_POOL_SIZE = 2;
        constexpr auto SHUTDOWN_GRACE = std::chrono::seconds(10);
        constexpr auto STOP_GRACE = std::chrono::seconds(5);

        [[nodiscard]]
        auto resolve_pool_size(int max_concurrent) -> std::size_t {
            return std::max<std::size_t>(static_cast<std::size_t>(std::max(1, max_concurrent)), MIN_POOL_SIZE);
        }

        [[nodiscard]]
        constexpr auto counts_toward_concurrency(run_status status) -> bool {
            return status == run_status::running || status == run_status::idle;
        }

        [[nodiscard]]
        constexpr auto is_active_run_status(run_status status) -> bool {
            return status == run_status::queued || counts_toward_concurrency(status);
        }

        [[nodiscard]]
        auto format_task_notification(const AgentRunRecord &record) -> std::string {
            return fmt::format(
                "<task-notification>\n"
                "  <task-id>{}</task-id>\n"
                "  <agent-key>{}</agent-key>\n"
                "  <agent-name>{}</agent-name>\n"
                "  <role>{}</role>\n"
                "  <status>{}</status>\n"
                "  <summary>{}</summary>\n"
                "  <result>{}</result>\n"
                "  <error>{}</error>\n"
                "</task-notification>",
                utils::escape_xml(record.run_id),
                utils::escape_xml(record.agent_key),
                utils::escape_xml(record.agent_name),
                magic_enum::enum_name(record.role),
                magic_enum::enum_name(record.status),
                utils::escape_xml(record.task_summary),
                utils::escape_xml(record.final_output),
                utils::escape_xml(record.error));
        }

    } // namespace

    struct OrchestrationManager::Impl {
        struct ActiveRun {
            AgentSpawnRequest request;
            AgentRunRecord record;
            std::stop_source stop_source;
            mutable std::mutex mutex;
            std::condition_variable_any cv;
            bool completed = false;
        };

        const int max_concurrent;
        utils::TaskPool pool;
        exec::async_scope scope;

        mutable std::mutex mutex;
        AgentExecutionEnvironment env;
        TaskNotificationCallback notification_callback;
        std::unordered_map<std::string, RuntimeNotificationHandler> runtime_notification_handlers;
        WorkerRuntimeFactory worker_runtime_factory;
        std::unordered_map<std::string, std::shared_ptr<ActiveRun>> active_runs;
        std::deque<std::string> pending_run_ids;
        int running_count = 0;
        std::uint64_t next_run_id = 0;
        bool shutting_down = false;

        explicit Impl(int max_concurrent_agents)
        : max_concurrent(std::max(1, max_concurrent_agents)),
          pool(resolve_pool_size(max_concurrent_agents)) {}

        [[nodiscard]]
        auto make_run_id() -> std::string {
            auto seq = next_run_id++;
            return "run-" + std::to_string(utils::epoch_millis()) + "-" + std::to_string(seq);
        }

        /// Mutate run record and broadcast completion under run->mutex. Returns the
        /// post-mutation record so the caller can deliver notifications unlocked.
        template <typename Fn>
        auto with_run_locked(ActiveRun &run, Fn &&fn) -> AgentRunRecord {
            std::scoped_lock lock(run.mutex);
            std::forward<Fn>(fn)(run);
            return run.record;
        }

        /// Mark the run completed (terminated/failed/etc.) and wake any waiters.
        auto finalize_run(ActiveRun &run, run_status status, std::string error = {}) -> AgentRunRecord {
            return with_run_locked(run, [&](ActiveRun &r) {
                r.record.status = status;
                r.record.completed_at = utils::epoch_millis();
                r.record.error = std::move(error);
                r.completed = true;
                r.cv.notify_all();
            });
        }

        void launch_run_locked(const std::shared_ptr<ActiveRun> &run) {
            {
                std::scoped_lock rlock(run->mutex);
                run->record.status = run_status::running;
            }
            ++running_count;
            scope.spawn(stdexec::schedule(pool.scheduler()) | stdexec::then([this, run] {
                run_lifecycle(*run);
                release_slot_and_resume();
            }));
        }

        void release_slot_and_resume() {
            std::vector<std::shared_ptr<ActiveRun>> to_launch;
            {
                std::scoped_lock lock(mutex);
                --running_count;
                while (!pending_run_ids.empty() && running_count + static_cast<int>(to_launch.size()) < max_concurrent) {
                    auto run_id = pending_run_ids.front();
                    pending_run_ids.pop_front();
                    const auto it = active_runs.find(run_id);
                    if (it == active_runs.end()) {
                        continue;
                    }
                    std::scoped_lock rlock(it->second->mutex);
                    if (it->second->completed || it->second->record.status != run_status::queued) {
                        continue;
                    }
                    to_launch.push_back(it->second);
                }
                for (const auto &run : to_launch) {
                    launch_run_locked(run);
                }
            }
        }

        /// Core worker lifecycle: run → (optional idle loop) → terminal status → notify.
        void run_lifecycle(ActiveRun &run) {
            const auto &run_id = run.record.run_id;
            const auto role = run.record.role;
            const auto stop_token = run.stop_source.get_token();
            spdlog::info("agent worker started: id={} key={} role={}", run_id, run.record.agent_key, magic_enum::enum_name(role));

            if (stop_token.stop_requested()) {
                finalize_run(run, run_status::terminated);
                spdlog::info("agent worker terminated before execution: id={}", run_id);
                deliver_notification(run);
                return;
            }

            WorkerRuntimeFactory factory;
            {
                std::scoped_lock lock(mutex);
                factory = worker_runtime_factory;
            }

            if (factory == nullptr) {
                finalize_run(run, run_status::failed, "No worker runtime factory configured");
                spdlog::warn("agent worker failed (no factory): id={}", run_id);
                deliver_notification(run);
                return;
            }

            std::unique_ptr<WorkerRuntime> worker;
            try {
                worker = factory(run.request);
            } catch (const std::exception &e) {
                finalize_run(run, run_status::failed, e.what());
                deliver_notification(run);
                return;
            }

            if (!run_first_task(run, *worker, stop_token)) {
                deliver_notification(run);
                return;
            }

            if (is_teammate(role) && worker->is_persistent() && !stop_token.stop_requested()) {
                run_idle_loop(run, *worker, stop_token);
            }

            {
                std::scoped_lock lock(run.mutex);
                if (!run.completed) {
                    run.record.status = stop_token.stop_requested() ? run_status::terminated : run_status::succeeded;
                    run.record.completed_at = utils::epoch_millis();
                    run.completed = true;
                    run.cv.notify_all();
                }
            }
            spdlog::info("agent worker completed: id={}", run_id);
            deliver_notification(run);
        }

        /// Executes the initial task. Returns false if execution threw (run already finalized).
        [[nodiscard]]
        bool run_first_task(ActiveRun &run, WorkerRuntime &worker, const std::stop_token &stop_token) {
            try {
                auto output = worker.run(run.request.task_prompt, stop_token);
                std::scoped_lock lock(run.mutex);
                run.record.final_output = std::move(output);
                return true;
            } catch (const std::exception &e) {
                finalize_run(run, run_status::failed, e.what());
                return false;
            }
        }

        /// Teammate idle loop: mark idle, notify leader, wait for next prompt, repeat.
        void run_idle_loop(ActiveRun &run, WorkerRuntime &worker, const std::stop_token &stop_token) {
            {
                std::scoped_lock lock(run.mutex);
                run.record.status = run_status::idle;
            }
            spdlog::info("teammate entering idle loop: id={}", run.record.run_id);
            deliver_notification(run);

            while (!stop_token.stop_requested()) {
                auto next_prompt = worker.wait_for_next_prompt(stop_token);
                if (!next_prompt.has_value()) {
                    spdlog::info("teammate exiting idle loop (no more prompts): id={}", run.record.run_id);
                    return;
                }

                {
                    std::scoped_lock lock(run.mutex);
                    run.record.status = run_status::running;
                    run.record.task_summary = *next_prompt;
                }
                spdlog::info("teammate resuming with new prompt: id={}", run.record.run_id);

                try {
                    auto output = worker.run(*next_prompt, stop_token);
                    std::scoped_lock lock(run.mutex);
                    run.record.final_output = std::move(output);
                    run.record.status = run_status::idle;
                } catch (const std::exception &e) {
                    finalize_run(run, run_status::failed, e.what());
                    return;
                }
                deliver_notification(run);
            }
        }

        /// Deliver task-notification via both runtime handler and global callback.
        /// Copies callback targets and record out of the locks before invoking them.
        void deliver_notification(const ActiveRun &run) {
            TaskNotificationCallback callback;
            RuntimeNotificationHandler runtime_handler;
            {
                std::scoped_lock lock(mutex);
                callback = notification_callback;
                if (!run.record.parent_runtime_key.empty()) {
                    const auto it = runtime_notification_handlers.find(run.record.parent_runtime_key);
                    if (it != runtime_notification_handlers.end()) {
                        runtime_handler = it->second;
                    }
                }
            }

            AgentRunRecord record_snapshot;
            std::string notification_xml;
            {
                std::scoped_lock rlock(run.mutex);
                record_snapshot = run.record;
                notification_xml = format_task_notification(run.record);
            }

            if (runtime_handler != nullptr) {
                if (const auto error = runtime_handler(notification_xml); error.has_value()) {
                    spdlog::warn("failed to resume runtime {} with task notification: {}", record_snapshot.parent_runtime_key, *error);
                }
            }
            if (callback != nullptr) {
                callback(record_snapshot);
            }
            spdlog::debug("task notification: {}", notification_xml);
        }
    };

    OrchestrationManager::OrchestrationManager(int max_concurrent_agents)
    : impl_(std::make_unique<Impl>(max_concurrent_agents)) {}

    OrchestrationManager::~OrchestrationManager() {
        shutdown();
    }

    void OrchestrationManager::set_environment(AgentExecutionEnvironment env) {
        std::scoped_lock lock(impl_->mutex);
        impl_->env = std::move(env);
    }

    void OrchestrationManager::set_notification_callback(TaskNotificationCallback callback) {
        std::scoped_lock lock(impl_->mutex);
        impl_->notification_callback = std::move(callback);
    }

    void OrchestrationManager::set_worker_runtime_factory(WorkerRuntimeFactory factory) {
        std::scoped_lock lock(impl_->mutex);
        impl_->worker_runtime_factory = std::move(factory);
    }

    void OrchestrationManager::register_runtime_notification_handler(std::string runtime_key, RuntimeNotificationHandler handler) {
        if (runtime_key.empty() || handler == nullptr) {
            return;
        }
        std::scoped_lock lock(impl_->mutex);
        impl_->runtime_notification_handlers.insert_or_assign(std::move(runtime_key), std::move(handler));
    }

    void OrchestrationManager::unregister_runtime_notification_handler(const std::string &runtime_key) {
        std::scoped_lock lock(impl_->mutex);
        impl_->runtime_notification_handlers.erase(runtime_key);
    }

    auto OrchestrationManager::spawn(const AgentSpawnRequest &request) -> AgentSpawnResult {
        std::scoped_lock lock(impl_->mutex);

        if (impl_->shutting_down) {
            return AgentSpawnResult{.accepted = false, .error = "Orchestration manager is shutting down"};
        }
        if (impl_->env.definition_registry != nullptr && !impl_->env.definition_registry->has(request.agent_key)) {
            return AgentSpawnResult{.accepted = false, .error = "Unknown agent key: " + request.agent_key};
        }

        auto run_id = impl_->make_run_id();
        auto agent_name = request.agent_name.empty() ? request.agent_key + "-" + std::to_string(impl_->next_run_id) : request.agent_name;

        auto active_run = std::make_shared<Impl::ActiveRun>();
        active_run->request = request;
        active_run->request.agent_name = agent_name;
        active_run->record = AgentRunRecord{
            .run_id = run_id,
            .agent_key = request.agent_key,
            .agent_name = agent_name,
            .team_id = request.team_id,
            .parent_runtime_key = request.parent_runtime_key,
            .role = request.role,
            .status = run_status::queued,
            .task_summary = request.task_prompt,
            .started_at = utils::epoch_millis(),
        };

        auto [it, inserted] = impl_->active_runs.try_emplace(run_id, std::move(active_run));
        if (impl_->running_count < impl_->max_concurrent) {
            impl_->launch_run_locked(it->second);
        } else {
            impl_->pending_run_ids.push_back(run_id);
        }

        spdlog::info("spawned agent run: id={} key={} name={} role={}", run_id, request.agent_key, agent_name, magic_enum::enum_name(request.role));
        return AgentSpawnResult{.accepted = true, .run_id = run_id, .agent_name = agent_name};
    }

    auto OrchestrationManager::send_message(const std::string &run_id, const std::string &from, const std::string &text) -> std::optional<std::string> {
        AgentMailbox *mailbox = nullptr;
        std::string team_id;
        std::string recipient;
        {
            std::scoped_lock lock(impl_->mutex);

            const auto it = impl_->active_runs.find(run_id);
            if (it == impl_->active_runs.end()) {
                spdlog::warn("send_message: unknown run_id={}", run_id);
                return "Unknown run_id: " + run_id;
            }
            if (impl_->env.mailbox == nullptr) {
                return "Mailbox is not available";
            }

            std::scoped_lock run_lock(it->second->mutex);
            const auto &record = it->second->record;
            if (record.team_id.empty()) {
                return "Target run is not attached to a team mailbox";
            }
            mailbox = impl_->env.mailbox;
            team_id = record.team_id;
            recipient = record.agent_name;
        }
        mailbox->send(team_id, from, recipient, text);
        spdlog::debug("message sent to run_id={} from={}", run_id, from);
        return std::nullopt;
    }

    auto OrchestrationManager::send_message_by_name(const std::string &team_id, const std::string &from, const std::string &to, const std::string &text)
        -> std::optional<std::string> {
        if (impl_->env.mailbox == nullptr) {
            return "Mailbox is not available";
        }
        if (impl_->env.team_manager == nullptr) {
            return "Team manager is not available";
        }

        auto member_names = impl_->env.team_manager->list_member_names(team_id);
        if (std::ranges::find(member_names, to) == member_names.end()) {
            return "Agent '" + to + "' not found in team";
        }
        impl_->env.mailbox->send(team_id, from, to, text);
        spdlog::debug("message sent to agent={} in team={} from={}", to, team_id, from);
        return std::nullopt;
    }

    auto OrchestrationManager::broadcast_message(const std::string &team_id, const std::string &from, const std::string &text) -> std::optional<std::string> {
        if (impl_->env.mailbox == nullptr) {
            return "Mailbox is not available";
        }
        if (impl_->env.team_manager == nullptr) {
            return "Team manager is not available";
        }
        auto member_names = impl_->env.team_manager->list_member_names(team_id);
        impl_->env.mailbox->send_broadcast(team_id, from, text, member_names);
        spdlog::debug("broadcast sent in team={} from={}", team_id, from);
        return std::nullopt;
    }

    void OrchestrationManager::stop(const std::string &run_id) {
        std::shared_ptr<Impl::ActiveRun> run;
        bool stopped_queued = false;
        {
            std::scoped_lock lock(impl_->mutex);
            const auto it = impl_->active_runs.find(run_id);
            if (it == impl_->active_runs.end()) {
                spdlog::warn("stop: unknown run_id={}", run_id);
                return;
            }
            if (it->second->record.status == run_status::queued) {
                std::erase(impl_->pending_run_ids, run_id);
                impl_->finalize_run(*it->second, run_status::terminated);
                stopped_queued = true;
            } else {
                run = it->second;
            }
        }

        if (stopped_queued) {
            spdlog::info("stopped queued agent run: id={}", run_id);
            return;
        }

        run->stop_source.request_stop();
        {
            std::unique_lock lock(run->mutex);
            run->cv.wait_for(lock, STOP_GRACE, [&run] {
                return run->completed;
            });
            if (!run->completed) {
                run->record.status = run_status::terminated;
                run->record.completed_at = utils::epoch_millis();
                run->completed = true;
                run->cv.notify_all();
            }
        }
        spdlog::info("stopped agent run: id={}", run_id);
    }

    auto OrchestrationManager::get_run(const std::string &run_id) const -> std::optional<AgentRunRecord> {
        std::scoped_lock lock(impl_->mutex);
        const auto it = impl_->active_runs.find(run_id);
        if (it == impl_->active_runs.end()) {
            return std::nullopt;
        }
        std::scoped_lock run_lock(it->second->mutex);
        return it->second->record;
    }

    auto OrchestrationManager::list_active_runs() const -> std::vector<AgentRunRecord> {
        std::scoped_lock lock(impl_->mutex);
        std::vector<AgentRunRecord> result;
        result.reserve(impl_->active_runs.size());
        for (const auto &[id, run] : impl_->active_runs) {
            std::scoped_lock rlock(run->mutex);
            if (is_active_run_status(run->record.status)) {
                result.push_back(run->record);
            }
        }
        return result;
    }

    void OrchestrationManager::shutdown() {
        std::vector<std::shared_ptr<Impl::ActiveRun>> runs_to_stop;
        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->shutting_down) {
                return;
            }
            impl_->shutting_down = true;
            impl_->pending_run_ids.clear();
            for (auto &[id, run] : impl_->active_runs) {
                static_cast<void>(id);
                std::scoped_lock rlock(run->mutex);
                if (!run->completed && run->record.status == run_status::queued) {
                    run->record.status = run_status::terminated;
                    run->record.completed_at = utils::epoch_millis();
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
        for (auto &run : runs_to_stop) {
            std::unique_lock lock(run->mutex);
            run->cv.wait_for(lock, SHUTDOWN_GRACE, [&run] {
                return run->completed;
            });
            if (!run->completed) {
                run->record.status = run_status::abandoned;
                run->record.completed_at = utils::epoch_millis();
                run->completed = true;
            }
        }

        static_cast<void>(stdexec::sync_wait(impl_->scope.on_empty()));

        {
            std::scoped_lock lock(impl_->mutex);
            impl_->active_runs.clear();
        }
        spdlog::info("orchestration manager shutdown complete");
    }

} // namespace orangutan::orchestration
