#include "orchestration/orchestration-manager.hpp"

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
#include <optional>
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
        constexpr auto is_active_run_status(run_status status) -> bool {
            return status == run_status::queued || status == run_status::running || status == run_status::idle;
        }

        [[nodiscard]]
        auto format_task_notification(const AgentRunRecord &record) -> std::string {
            return fmt::format(
                "<task-notification>\n"
                "  <task-id>{}</task-id>\n"
                "  <agent-name>{}</agent-name>\n"
                "  <role>{}</role>\n"
                "  <relationship>{}</relationship>\n"
                "  <status>{}</status>\n"
                "  <summary>{}</summary>\n"
                "  <result>{}</result>\n"
                "  <error>{}</error>\n"
                "</task-notification>",
                utils::escape_xml(record.run_id),
                utils::escape_xml(record.agent_name),
                magic_enum::enum_name(record.role),
                magic_enum::enum_name(record.relationship),
                magic_enum::enum_name(record.status),
                utils::escape_xml(record.task_summary),
                utils::escape_xml(record.final_output),
                utils::escape_xml(record.error));
        }

        [[nodiscard]]
        auto format_initial_prompt(const AgentSpawnRequest &request) -> std::string {
            if (request.instructions.empty()) {
                return request.task;
            }
            return fmt::format("# Instructions\n{}\n\n# Task\n{}", request.instructions, request.task);
        }

    } // namespace

    struct OrchestrationManager::Impl {
        struct ActiveRun {
            AgentSpawnRequest request;
            AgentRunRecord record;
            std::unique_ptr<TeammateRuntime> teammate;
            std::optional<std::string> pending_prompt;
            std::stop_source stop_source;
            mutable std::mutex mutex;
            std::condition_variable_any cv;
            bool initial_task_started = false;
            bool completed = false;
        };

        const int max_concurrent;
        std::unique_ptr<utils::TaskPool> pool;
        exec::async_scope scope;

        mutable std::mutex mutex;
        AgentExecutionEnvironment env;
        TaskNotificationCallback notification_callback;
        std::unordered_map<std::string, RuntimeNotificationHandler> runtime_notification_handlers;
        TeammateRuntimeFactory teammate_runtime_factory;
        std::unordered_map<std::string, std::shared_ptr<ActiveRun>> active_runs;
        std::deque<std::string> pending_run_ids;
        int running_count = 0;
        std::uint64_t next_run_id = 0;
        bool shutting_down = false;

        explicit Impl(int max_concurrent_agents)
        : max_concurrent(std::max(1, max_concurrent_agents)) {}

        [[nodiscard]]
        auto scheduler() -> exec::static_thread_pool::scheduler {
            if (pool == nullptr) {
                pool = std::make_unique<utils::TaskPool>(resolve_pool_size(max_concurrent));
            }
            return pool->scheduler();
        }

        [[nodiscard]]
        auto make_run_id() -> std::string {
            auto seq = next_run_id++;
            return "run-" + std::to_string(utils::epoch_millis()) + "-" + std::to_string(seq);
        }

        [[nodiscard]]
        constexpr auto is_terminal_status(run_status status) -> bool {
            return status == run_status::succeeded || status == run_status::failed || status == run_status::terminated || status == run_status::abandoned;
        }

        /// Mark the run completed (terminated/failed/etc.) and wake any waiters.
        [[nodiscard]]
        bool finalize_run(ActiveRun &run, run_status status, std::string error = {}) {
            std::scoped_lock lock(run.mutex);
            if (run.completed && is_terminal_status(run.record.status)) {
                return false;
            }
            run.record.status = status;
            run.record.completed_at = utils::epoch_millis();
            run.record.error = std::move(error);
            run.completed = true;
            run.cv.notify_all();
            return true;
        }

        void finalize_and_notify(ActiveRun &run, run_status status, std::string error = {}) {
            if (finalize_run(run, status, std::move(error))) {
                deliver_notification(run);
            }
        }

        void launch_run_locked(const std::shared_ptr<ActiveRun> &run) {
            {
                std::scoped_lock rlock(run->mutex);
                run->record.status = run_status::running;
            }
            ++running_count;
            scope.spawn(stdexec::schedule(scheduler()) | stdexec::then([this, run] {
                run_lifecycle(*run);
                release_slot_and_resume();
            }));
        }

        void launch_queued_runs_locked() {
            while (!pending_run_ids.empty() && running_count < max_concurrent) {
                auto run_id = pending_run_ids.front();
                pending_run_ids.pop_front();
                const auto it = active_runs.find(run_id);
                if (it == active_runs.end()) {
                    continue;
                }
                bool should_launch = false;
                {
                    std::scoped_lock rlock(it->second->mutex);
                    should_launch = !it->second->completed && it->second->record.status == run_status::queued;
                }
                if (should_launch) {
                    launch_run_locked(it->second);
                }
            }
        }

        void release_slot_and_resume() {
            std::scoped_lock lock(mutex);
            --running_count;
            launch_queued_runs_locked();
        }

        void queue_idle_runs_for_message(std::string_view team_id, std::optional<std::string_view> recipient) {
            std::scoped_lock lock(mutex);
            for (const auto &[id, run] : active_runs) {
                static_cast<void>(id);
                std::scoped_lock rlock(run->mutex);
                if (run->completed || run->record.status != run_status::idle || run->record.team_id != team_id) {
                    continue;
                }
                if (recipient.has_value() && run->record.agent_name != *recipient) {
                    continue;
                }
                run->record.status = run_status::queued;
                pending_run_ids.push_back(run->record.run_id);
            }
            launch_queued_runs_locked();
        }

        /// Run one teammate prompt. Idle teammates release the execution slot until mailbox messages wake them.
        void run_lifecycle(ActiveRun &run) {
            const auto &run_id = run.record.run_id;
            const auto stop_token = run.stop_source.get_token();
            spdlog::info("teammate started: id={} name={} relationship={}", run_id, run.record.agent_name, magic_enum::enum_name(run.record.relationship));

            if (is_completed(run)) {
                return;
            }
            if (stop_token.stop_requested()) {
                finalize_and_notify(run, run_status::terminated);
                spdlog::info("teammate terminated before execution: id={}", run_id);
                return;
            }

            if (!ensure_teammate(run)) {
                return;
            }

            auto prompt = take_next_prompt(run);
            if (!prompt.has_value()) {
                mark_idle(run);
                spdlog::info("teammate returned to idle without pending prompt: id={}", run_id);
                return;
            }

            if (!run_prompt(run, *prompt, stop_token)) {
                return;
            }

            finish_prompt(run, stop_token);
        }

        [[nodiscard]]
        bool is_completed(const ActiveRun &run) const {
            std::scoped_lock lock(run.mutex);
            return run.completed;
        }

        [[nodiscard]]
        bool ensure_teammate(ActiveRun &run) {
            {
                std::scoped_lock lock(run.mutex);
                if (run.teammate != nullptr) {
                    return true;
                }
            }

            TeammateRuntimeFactory factory;
            {
                std::scoped_lock lock(mutex);
                factory = teammate_runtime_factory;
            }

            if (factory == nullptr) {
                finalize_and_notify(run, run_status::failed, "No teammate runtime factory configured");
                spdlog::warn("teammate failed (no factory): id={}", run.record.run_id);
                return false;
            }

            std::unique_ptr<TeammateRuntime> teammate;
            try {
                teammate = factory(run.request);
            } catch (const std::exception &e) {
                finalize_and_notify(run, run_status::failed, e.what());
                return false;
            }

            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return false;
                }
                run.teammate = std::move(teammate);
            }
            return true;
        }

        [[nodiscard]]
        std::optional<std::string> take_next_prompt(ActiveRun &run) {
            TeammateRuntime *teammate = nullptr;
            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return std::nullopt;
                }
                if (!run.initial_task_started) {
                    run.initial_task_started = true;
                    return format_initial_prompt(run.request);
                }
                if (run.pending_prompt.has_value()) {
                    auto prompt = std::move(run.pending_prompt);
                    run.pending_prompt.reset();
                    return prompt;
                }
                teammate = run.teammate.get();
            }
            if (teammate == nullptr) {
                return std::nullopt;
            }
            return teammate->poll_next_prompt();
        }

        [[nodiscard]]
        bool run_prompt(ActiveRun &run, const std::string &prompt, const std::stop_token &stop_token) {
            TeammateRuntime *teammate = nullptr;
            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return false;
                }
                teammate = run.teammate.get();
                run.record.status = run_status::running;
                run.record.task_summary = prompt;
            }
            if (teammate == nullptr) {
                finalize_and_notify(run, run_status::failed, "No teammate runtime available");
                return false;
            }

            try {
                auto output = teammate->run(prompt, stop_token);
                std::scoped_lock lock(run.mutex);
                if (!run.completed) {
                    run.record.final_output = std::move(output);
                }
                return true;
            } catch (const std::exception &e) {
                finalize_and_notify(run, run_status::failed, e.what());
                return false;
            }
        }

        void mark_idle(ActiveRun &run) {
            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return;
                }
                run.record.status = run_status::idle;
                run.cv.notify_all();
            }
        }

        void finish_prompt(ActiveRun &run, const std::stop_token &stop_token) {
            if (stop_token.stop_requested()) {
                finalize_and_notify(run, run_status::terminated);
                return;
            }

            TeammateRuntime *teammate = nullptr;
            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return;
                }
                teammate = run.teammate.get();
            }

            if (teammate == nullptr || !teammate->can_receive_followups()) {
                finalize_and_notify(run, run_status::succeeded);
                spdlog::info("teammate completed: id={}", run.record.run_id);
                return;
            }

            auto pending_prompt = teammate->poll_next_prompt();
            std::string run_id;
            bool requeued = false;
            {
                std::scoped_lock lock(run.mutex);
                if (run.completed) {
                    return;
                }
                run_id = run.record.run_id;
                if (pending_prompt.has_value()) {
                    run.pending_prompt = std::move(pending_prompt);
                    run.record.status = run_status::queued;
                    requeued = true;
                } else {
                    run.record.status = run_status::idle;
                }
                run.cv.notify_all();
            }
            if (requeued) {
                std::scoped_lock lock(mutex);
                pending_run_ids.push_back(run_id);
            }
            spdlog::info("teammate idle: id={}", run_id);
            deliver_notification(run);
        }

        void stop_run(const std::shared_ptr<ActiveRun> &run, std::chrono::milliseconds grace_period) {
            bool should_notify = false;
            {
                std::scoped_lock lock(mutex);
                std::scoped_lock rlock(run->mutex);
                if (run->completed) {
                    return;
                }
                if (run->record.status == run_status::queued || run->record.status == run_status::idle) {
                    std::erase(pending_run_ids, run->record.run_id);
                    run->record.status = run_status::terminated;
                    run->record.completed_at = utils::epoch_millis();
                    run->completed = true;
                    run->cv.notify_all();
                    should_notify = true;
                }
            }

            if (should_notify) {
                deliver_notification(*run);
                return;
            }

            run->stop_source.request_stop();
            {
                std::unique_lock lock(run->mutex);
                run->cv.wait_for(lock, grace_period, [&run] {
                    return run->completed;
                });
                if (!run->completed) {
                    run->record.status = run_status::terminated;
                    run->record.completed_at = utils::epoch_millis();
                    run->completed = true;
                    run->cv.notify_all();
                    should_notify = true;
                }
            }
            if (should_notify) {
                deliver_notification(*run);
            }
        }

        [[nodiscard]]
        std::vector<std::shared_ptr<ActiveRun>> active_team_runs(const std::string &team_id) const {
            std::vector<std::shared_ptr<ActiveRun>> result;
            std::scoped_lock lock(mutex);
            for (const auto &[id, run] : active_runs) {
                static_cast<void>(id);
                std::scoped_lock rlock(run->mutex);
                if (!run->completed && is_active_run_status(run->record.status) && run->record.team_id == team_id) {
                    result.push_back(run);
                }
            }
            return result;
        }

        [[nodiscard]]
        std::shared_ptr<ActiveRun> find_run(const std::string &run_id) const {
            std::scoped_lock lock(mutex);
            const auto it = active_runs.find(run_id);
            if (it == active_runs.end()) {
                return nullptr;
            }
            return it->second;
        }

        [[nodiscard]]
        std::optional<std::string> validate_run_can_receive(const std::shared_ptr<ActiveRun> &run) const {
            std::scoped_lock lock(run->mutex);
            if (run->completed || !is_active_run_status(run->record.status)) {
                return "Target run is no longer active";
            }
            if (run->record.team_id.empty()) {
                return "Target run is not attached to a team mailbox";
            }
            return std::nullopt;
        }

        [[nodiscard]]
        std::shared_ptr<ActiveRun> find_active_team_run_by_name(const std::string &team_id, const std::string &agent_name) const {
            std::scoped_lock lock(mutex);
            for (const auto &[id, run] : active_runs) {
                static_cast<void>(id);
                std::scoped_lock rlock(run->mutex);
                if (!run->completed && is_active_run_status(run->record.status) && run->record.team_id == team_id && run->record.agent_name == agent_name) {
                    return run;
                }
            }
            return nullptr;
        }

        void deactivate_team_member(const AgentRunRecord &record) {
            TeamManager *manager = nullptr;
            {
                std::scoped_lock lock(mutex);
                manager = env.team_manager;
            }
            if (manager == nullptr || record.team_id.empty() || record.run_id.empty()) {
                return;
            }
            manager->deactivate_member(record.team_id, record.run_id);
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

            if (!is_active_run_status(record_snapshot.status)) {
                deactivate_team_member(record_snapshot);
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

    void OrchestrationManager::set_teammate_runtime_factory(TeammateRuntimeFactory factory) {
        std::scoped_lock lock(impl_->mutex);
        impl_->teammate_runtime_factory = std::move(factory);
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

        auto run_id = impl_->make_run_id();
        auto agent_name = request.name.empty() ? "agent-" + std::to_string(impl_->next_run_id) : request.name;

        auto active_run = std::make_shared<Impl::ActiveRun>();
        active_run->request = request;
        active_run->request.name = agent_name;
        active_run->record = AgentRunRecord{
            .run_id = run_id,
            .agent_name = agent_name,
            .team_id = request.team_id,
            .parent_runtime_key = request.parent_runtime_key,
            .role = agent_role::teammate,
            .relationship = request.relationship,
            .status = run_status::queued,
            .task_summary = request.task,
            .started_at = utils::epoch_millis(),
        };

        auto [it, inserted] = impl_->active_runs.try_emplace(run_id, std::move(active_run));
        if (impl_->running_count < impl_->max_concurrent) {
            impl_->launch_run_locked(it->second);
        } else {
            impl_->pending_run_ids.push_back(run_id);
        }

        spdlog::info("spawned teammate run: id={} name={} relationship={}", run_id, agent_name, magic_enum::enum_name(request.relationship));
        return AgentSpawnResult{.accepted = true, .run_id = run_id, .agent_name = agent_name};
    }

    auto OrchestrationManager::send_message(const std::string &run_id, const std::string &from, const std::string &text) -> std::optional<std::string> {
        AgentMailbox *mailbox = nullptr;
        std::string team_id;
        std::string recipient;
        auto run = impl_->find_run(run_id);
        if (run == nullptr) {
            spdlog::warn("send_message: unknown run_id={}", run_id);
            return "Unknown run_id: " + run_id;
        }
        if (const auto error = impl_->validate_run_can_receive(run); error.has_value()) {
            return error;
        }
        {
            std::scoped_lock lock(impl_->mutex);
            if (impl_->env.mailbox == nullptr) {
                return "Mailbox is not available";
            }
            mailbox = impl_->env.mailbox;
        }
        {
            std::scoped_lock run_lock(run->mutex);
            const auto &record = run->record;
            team_id = record.team_id;
            recipient = record.agent_name;
        }
        mailbox->send(team_id, from, recipient, text);
        impl_->queue_idle_runs_for_message(team_id, recipient);
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
        const auto run = impl_->find_active_team_run_by_name(team_id, to);
        if (run == nullptr) {
            return "Agent '" + to + "' is not running in team";
        }
        impl_->env.mailbox->send(team_id, from, to, text);
        impl_->queue_idle_runs_for_message(team_id, to);
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
        impl_->queue_idle_runs_for_message(team_id, std::nullopt);
        spdlog::debug("broadcast sent in team={} from={}", team_id, from);
        return std::nullopt;
    }

    void OrchestrationManager::stop(const std::string &run_id) {
        auto run = impl_->find_run(run_id);
        if (run == nullptr) {
            spdlog::warn("stop: unknown run_id={}", run_id);
            return;
        }
        impl_->stop_run(run, STOP_GRACE);
        spdlog::info("stopped agent run: id={}", run_id);
    }

    std::size_t OrchestrationManager::stop_team(const std::string &team_id, std::chrono::milliseconds grace_period) {
        auto runs = impl_->active_team_runs(team_id);
        for (const auto &run : runs) {
            impl_->stop_run(run, grace_period);
        }
        return runs.size();
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
            for (auto &[id, run] : impl_->active_runs) {
                static_cast<void>(id);
                runs_to_stop.push_back(run);
            }
        }

        for (const auto &run : runs_to_stop) {
            impl_->stop_run(run, SHUTDOWN_GRACE);
        }

        static_cast<void>(stdexec::sync_wait(impl_->scope.on_empty()));

        {
            std::scoped_lock lock(impl_->mutex);
            impl_->active_runs.clear();
        }
        spdlog::info("orchestration manager shutdown complete");
    }

} // namespace orangutan::orchestration
