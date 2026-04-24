#include "automation/service.hpp"

#include "automation/kernel.hpp"
#include "automation/log-writer.hpp"
#include "automation/sqlite-store.hpp"

#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::automation {
    namespace {

        [[nodiscard]]
        auto failed_execution(std::string_view message) -> ExecutionResult {
            return ExecutionResult{
                .success = false,
                .summary = std::string(message),
            };
        }

        [[nodiscard]]
        auto to_schedule_spec(const TriggerDefinition &trigger) -> ScheduleSpec {
            switch (trigger.type) {
            case trigger_type::cron:
                return CronSchedule{
                    .expr = trigger.cron,
                    .time_zone = trigger.time_zone,
                };
            case trigger_type::interval:
                return IntervalSchedule{
                    .every = trigger.every,
                    .jitter = trigger.jitter,
                    .active_windows = trigger.active_windows,
                    .time_zone = trigger.time_zone,
                };
            case trigger_type::once:
                return OneShotSchedule{
                    .at = trigger.at,
                };
            }

            std::unreachable();
        }

        [[nodiscard]]
        auto to_job_definition(const Automation &automation) -> JobDefinition {
            return JobDefinition{
                .id = JobId{.value = automation.id},
                .key = automation.name,
                .schedule = to_schedule_spec(automation.trigger),
                .action =
                    ActionDescriptor{
                        .action_key = "legacy.automation",
                        .payload =
                            {
                                {"agent_key", automation.agent_key},
                                {"name", automation.name},
                                {"prompt", automation.prompt},
                            },
                    },
                .execution = ExecutionPolicy{},
                .result =
                    ResultPolicy{
                        .mode = automation.delivery.mode,
                        .targets = automation.delivery.targets,
                    },
                .metadata =
                    {
                        {"notes", automation.notes},
                        {"tags", automation.tags},
                    },
            };
        }

        [[nodiscard]]
        auto to_schedule_state(const Automation &automation) -> ScheduleState {
            return ScheduleState{
                .enabled = automation.enabled,
                .paused = automation.paused,
                .next_due_at = automation.next_due_at,
                .last_finished_at = automation.last_run_at,
                .last_status = automation.last_status,
            };
        }

        void throw_on_store_error(const StoreResult<void> &result, std::string_view context) {
            if (result.has_value()) {
                return;
            }
            throw std::runtime_error(std::string(context) + ": " + result.error().message);
        }

        void throw_on_store_error(const StoreResult<bool> &result, std::string_view context) {
            if (result.has_value()) {
                return;
            }
            throw std::runtime_error(std::string(context) + ": " + result.error().message);
        }

    } // namespace

    AutomationService::AutomationService(Repository &repository, ClockSource clock)
    : repository_(&repository),
      clock_(std::move(clock)),
      core_store_(std::make_unique<SqliteJobStore>(repository.db_path())),
      core_kernel_(std::make_unique<Kernel>(*core_store_)) {
        sync_existing_core_jobs();
    }

    AutomationService::~AutomationService() = default;

    void AutomationService::set_executor(AutomationExecutor executor) {
        std::scoped_lock lock(mutex_);
        executor_ = std::move(executor);
    }

    void AutomationService::add_delivery_filter(AutomationDeliveryFilter filter) {
        std::scoped_lock lock(mutex_);
        delivery_filters_.push_back(std::move(filter));
    }

    void AutomationService::register_category(AutomationCategory category) {
        if (category.delivery_filter != nullptr) {
            add_delivery_filter(std::move(category.delivery_filter));
        }
    }

    void AutomationService::set_notifier(AutomationNotifier notifier) {
        std::scoped_lock lock(mutex_);
        notifier_ = std::move(notifier);
    }

    std::string AutomationService::save(Automation automation) {
        if (automation.id.empty()) {
            automation.id = generate_id("auto");
        }
        return persist(normalize_public_save(std::move(automation), current_time()));
    }

    std::vector<Automation> AutomationService::list(const AutomationQuery &query) const {
        return repository_->list(query);
    }

    std::optional<Automation> AutomationService::find(std::string_view agent_key, std::string_view id_or_name) const {
        return repository_->find(agent_key, id_or_name);
    }

    bool AutomationService::remove(std::string_view agent_key, std::string_view id_or_name) {
        const auto automation = repository_->find(agent_key, id_or_name);
        if (!automation.has_value()) {
            return false;
        }

        if (!repository_->remove(agent_key, id_or_name)) {
            return false;
        }

        throw_on_store_error(core_store_->remove_job(JobId{.value = automation->id}), "failed to remove automation core job");
        return true;
    }

    std::string AutomationService::run_now(std::string_view agent_key, std::string_view id_or_name) {
        const auto automation = repository_->find(agent_key, id_or_name);
        if (!automation.has_value()) {
            throw std::runtime_error("automation not found");
        }

        return execute(*automation, current_time());
    }

    bool AutomationService::pause(std::string_view agent_key, std::string_view id_or_name) {
        auto automation = repository_->find(agent_key, id_or_name);
        if (!automation.has_value() || !automation->enabled) {
            return false;
        }

        automation->paused = true;
        static_cast<void>(persist(*automation));
        return true;
    }

    bool AutomationService::resume(std::string_view agent_key, std::string_view id_or_name) {
        auto automation = repository_->find(agent_key, id_or_name);
        if (!automation.has_value() || !automation->enabled) {
            return false;
        }

        automation->paused = false;
        automation->next_due_at = plan_next_due(*automation, current_time());
        static_cast<void>(persist(*automation));
        return true;
    }

    std::vector<RunRecord> AutomationService::list_runs(const RunQuery &query) const {
        return repository_->list_runs(query);
    }

    std::vector<DeliveryRecord> AutomationService::list_deliveries(const DeliveryQuery &query) const {
        return repository_->list_deliveries(query);
    }

    bool AutomationService::ack_delivery(std::string_view agent_key, std::string_view delivery_id) {
        return repository_->ack_delivery(agent_key, delivery_id).has_value();
    }

    std::string AutomationService::record_delivery(DeliveryRecord delivery) {
        if (delivery.created_at == 0) {
            delivery.created_at = to_unix_seconds(current_time());
        }
        return repository_->insert_delivery(delivery);
    }

    void AutomationService::clear_deliveries(const DeliveryQuery &query) {
        repository_->clear_deliveries(query);
    }

    TimePoint AutomationService::current_time() const {
        if (clock_ != nullptr) {
            return clock_();
        }

        return Clock::now();
    }

    AutomationService::Callbacks AutomationService::callbacks() const {
        std::scoped_lock lock(mutex_);
        return Callbacks{
            .executor = executor_,
            .delivery_filters = delivery_filters_,
            .notifier = notifier_,
        };
    }

    Automation AutomationService::normalize_public_save(Automation automation, TimePoint now) const {
        if (!automation.enabled && automation.paused) {
            automation.paused = false;
        }

        if (!automation.enabled) {
            automation.next_due_at.reset();
            return automation;
        }

        if (automation.trigger.type == trigger_type::once && automation.last_run_at.has_value()) {
            automation.next_due_at.reset();
            return automation;
        }

        if (!automation.paused) {
            automation.next_due_at = plan_next_due(automation, now);
        }

        return automation;
    }

    std::string AutomationService::persist(Automation automation) {
        automation.id = repository_->save(automation);
        sync_core_job(automation);
        return automation.id;
    }

    void AutomationService::sync_core_job(const Automation &automation) {
        throw_on_store_error(core_store_->save_job(to_job_definition(automation), to_schedule_state(automation)), "failed to sync automation core job");
    }

    void AutomationService::sync_existing_core_jobs() {
        for (const auto &automation : repository_->list()) {
            sync_core_job(automation);
        }
    }

    auto AutomationService::execute_outcome(const Automation &automation, TimePoint started_at, bool sync_core_state) -> ExecutionOutcome {
        const auto callbacks_snapshot = callbacks();
        auto result = ExecutionResult{};
        try {
            if (callbacks_snapshot.executor != nullptr) {
                result = callbacks_snapshot.executor(automation);
            } else {
                result = failed_execution("no automation executor configured");
            }
        } catch (const std::exception &error) {
            result = failed_execution(error.what());
        } catch (...) {
            result = failed_execution("unknown automation execution failure");
        }

        RunRecord run{
            .id = generate_id("run"),
            .automation_id = automation.id,
            .agent_key = automation.agent_key,
            .automation_name = automation.name,
            .started_at = to_unix_seconds(started_at),
        };

        const auto finished_at = to_unix_seconds(current_time());
        run.finished_at = finished_at;
        run.status = result.success ? "completed" : "failed";
        run.summary = result.summary;
        run.reply = result.reply;

        if (!result.workspace_root.empty()) {
            try {
                run.log_path = LogWriter::append_run(result.workspace_root, automation, run);
            } catch (const std::exception &) {
                run.log_path.clear();
            }
        }

        std::optional<DeliveryDisposition> delivery_disposition;
        for (const auto &filter : callbacks_snapshot.delivery_filters) {
            if (filter == nullptr) {
                continue;
            }
            if (auto decision = filter(automation, result); decision.has_value()) {
                delivery_disposition = std::move(decision);
                break;
            }
        }

        auto updated = automation;
        updated.last_run_at = finished_at;
        updated.last_status = run.status;
        if (updated.trigger.type == trigger_type::once) {
            updated.next_due_at.reset();
        } else {
            updated.next_due_at = plan_next_due(updated, from_unix_seconds(finished_at));
        }

        if (delivery_disposition.has_value() && delivery_disposition->suppress) {
            run.delivery_status = delivery_disposition->status.empty() ? "suppressed" : delivery_disposition->status;
            repository_->persist_execution(updated, run);
        } else if (automation.delivery.mode == delivery_mode::silent) {
            run.delivery_status = "silent";
            repository_->persist_execution(updated, run);
        } else {
            run.delivery_status = "notify_pending";
            repository_->persist_execution(updated, run);

            const auto message = make_delivery_message(automation, result);
            auto delivery_status = std::string("notified");
            std::vector<DeliveryRecord> deliveries;
            deliveries.reserve(automation.delivery.targets.size());

            for (const auto &target : automation.delivery.targets) {
                auto delivery = DeliveryRecord{
                    .run_id = run.id,
                    .automation_id = automation.id,
                    .agent_key = automation.agent_key,
                    .target = target,
                    .status = "notified",
                    .title = message.title,
                    .body = message.body,
                    .created_at = finished_at,
                };

                if (callbacks_snapshot.notifier == nullptr) {
                    delivery.status = "notify_failed";
                    delivery_status = "notify_failed";
                } else {
                    try {
                        if (const auto error = callbacks_snapshot.notifier(target, delivery.title, delivery.body); error.has_value()) {
                            delivery.status = "notify_failed";
                            delivery.body = *error + "\n" + delivery.body;
                            delivery_status = "notify_failed";
                        }
                    } catch (const std::exception &error) {
                        delivery.status = "notify_failed";
                        delivery.body = std::string(error.what()) + "\n" + delivery.body;
                        delivery_status = "notify_failed";
                    } catch (...) {
                        delivery.status = "notify_failed";
                        delivery.body = "unknown notification failure\n" + delivery.body;
                        delivery_status = "notify_failed";
                    }
                }

                deliveries.push_back(std::move(delivery));
            }

            try {
                repository_->persist_delivery_results(run.id, delivery_status, deliveries);
            } catch (const std::exception &error) {
                spdlog::error("failed to persist delivery results for automation {} run {}: {}", automation.id, run.id, error.what());
            } catch (...) {
                spdlog::error("failed to persist delivery results for automation {} run {} with unknown exception", automation.id, run.id);
            }
        }

        if (sync_core_state) {
            sync_core_job(updated);
        }

        return ExecutionOutcome{
            .run_id = run.id,
            .result = std::move(result),
        };
    }

    std::string AutomationService::execute(const Automation &automation, TimePoint started_at) {
        return execute_outcome(automation, started_at, true).run_id;
    }

    std::vector<DueAutomation> AutomationService::collect_due(TimePoint now) const {
        const auto automations = repository_->list(AutomationQuery{
            .enabled = true,
            .paused = false,
        });
        return collect_due_automations(automations, now);
    }

    void AutomationService::normalize_state(TimePoint now) {
        const auto now_seconds = to_unix_seconds(now);
        auto automations = repository_->list(AutomationQuery{.enabled = true});

        for (auto &automation : automations) {
            if (automation.paused) {
                continue;
            }

            if (automation.trigger.type == trigger_type::once && automation.last_run_at.has_value()) {
                automation.next_due_at.reset();
                static_cast<void>(persist(automation));
                continue;
            }

            if (automation.next_due_at.has_value() && *automation.next_due_at >= now_seconds) {
                continue;
            }

            automation.next_due_at = plan_next_due(automation, now);
            static_cast<void>(persist(automation));
        }
    }

    Kernel &AutomationService::core_kernel() noexcept {
        return *core_kernel_;
    }

    const Kernel &AutomationService::core_kernel() const noexcept {
        return *core_kernel_;
    }

} // namespace orangutan::automation
