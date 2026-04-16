#include "channel/jid-task-runner.hpp"

#include "utils/task-pool.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace orangutan::channel {

    struct JidTaskRunner::QueuedTask {
        virtual ~QueuedTask() = default;
        QueuedTask() = default;
        QueuedTask(const QueuedTask &) = delete;
        QueuedTask &operator=(const QueuedTask &) = delete;
        QueuedTask(QueuedTask &&) = delete;
        QueuedTask &operator=(QueuedTask &&) = delete;
        virtual void execute() noexcept = 0;
        virtual void cancel() noexcept = 0;
    };

    struct JidTaskRunner::SchedulerModel {
        struct Scheduler;

        template <class Receiver>
        struct Operation;

        struct Sender {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr), stdexec::set_stopped_t()>;

            JidTaskRunner *runner = nullptr;
            std::string jid;

            [[nodiscard]]
            auto get_env() const noexcept {
                return stdexec::prop{stdexec::get_completion_scheduler<stdexec::set_value_t>, Scheduler{.runner = runner, .jid = jid}};
            }

            template <class Receiver>
            auto connect(Receiver receiver) && -> Operation<std::decay_t<Receiver>> {
                return Operation<std::decay_t<Receiver>>{
                    .runner = runner,
                    .jid = std::move(jid),
                    .receiver = std::move(receiver),
                };
            }
        };

        struct Scheduler {
            JidTaskRunner *runner = nullptr;
            std::string jid;

            [[nodiscard]]
            auto schedule() const noexcept -> Sender {
                return Sender{
                    .runner = runner,
                    .jid = jid,
                };
            }

            auto operator==(const Scheduler &) const noexcept -> bool = default;
        };

        template <class Receiver>
        struct ReceiverTask final : QueuedTask {
            explicit ReceiverTask(Receiver receiver_arg)
            : receiver(std::move(receiver_arg)) {}

            void execute() noexcept override {
                stdexec::set_value(std::move(receiver));
            }

            void cancel() noexcept override {
                stdexec::set_stopped(std::move(receiver));
            }

            Receiver receiver;
        };

        template <class Receiver>
        struct Operation {
            using operation_state_concept = stdexec::operation_state_t;

            JidTaskRunner *runner = nullptr;
            std::string jid;
            std::optional<Receiver> receiver;

            void start() & noexcept {
                if (!receiver.has_value()) {
                    return;
                }

                if (runner == nullptr) {
                    stdexec::set_stopped(std::move(*receiver));
                    receiver.reset();
                    return;
                }

                try {
                    auto task = std::make_unique<ReceiverTask<Receiver>>(std::move(*receiver));
                    runner->enqueue_scheduled_task(jid, std::move(task));
                    receiver.reset();
                } catch (...) {
                    if (receiver.has_value()) {
                        stdexec::set_error(std::move(*receiver), std::current_exception());
                        receiver.reset();
                    }
                }
            }
        };
    };

    struct JidTaskRunner::Impl {
        utils::TaskPool pool;
        exec::async_scope scope;

        explicit Impl(std::size_t pool_size)
        : pool{pool_size} {}
    };

    namespace {

        constexpr std::size_t MIN_POOL_SIZE = 4;

        [[nodiscard]]
        std::size_t resolve_pool_size(std::size_t worker_count) {
            return std::max<std::size_t>(worker_count, MIN_POOL_SIZE);
        }

    } // namespace

    JidTaskRunner::JidTaskRunner(std::size_t worker_count)
    : worker_count_(worker_count),
      impl_(std::make_unique<Impl>(resolve_pool_size(worker_count))) {
        if (worker_count == 0) {
            throw std::invalid_argument("JidTaskRunner requires at least one worker");
        }
    }

    JidTaskRunner::~JidTaskRunner() {
        shutdown(true);
    }

    void JidTaskRunner::submit(std::string_view jid, Task task) {
        if (jid.empty()) {
            throw std::invalid_argument("JidTaskRunner requires a non-empty jid");
        }

        if (!task) {
            spdlog::debug("ignoring empty jidtaskrunner task for '{}'", jid);
            return;
        }

        auto pipeline =
            stdexec::schedule(SchedulerModel::Scheduler{.runner = this, .jid = std::string(jid)}) | stdexec::then([task = std::move(task), jid = std::string(jid)]() mutable {
                try {
                    task();
                } catch (const std::exception &e) {
                    spdlog::error("unhandled exception in jidtaskrunner task for '{}': {}", jid, e.what());
                } catch (...) {
                    spdlog::error("unhandled non-standard exception in jidtaskrunner task for '{}'", jid);
                }
            });
        stdexec::start_detached(std::move(pipeline));
    }

    void JidTaskRunner::enqueue_scheduled_task(std::string_view jid, std::unique_ptr<QueuedTask> task) {
        if (jid.empty()) {
            throw std::invalid_argument("JidTaskRunner requires a non-empty jid");
        }

        if (stopping_.load()) {
            task->cancel();
            return;
        }

        std::string activated_jid;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_.load()) {
                task->cancel();
                return;
            }

            auto &bucket = buckets_[std::string(jid)];
            bucket.tasks.push_back(std::move(task));
            if (bucket.active) {
                return;
            }

            bucket.active = true;
            activated_jid.assign(jid);
        }

        schedule_drain(std::move(activated_jid));
    }

    void JidTaskRunner::schedule_drain(std::string jid) {
        auto sender = stdexec::schedule(impl_->pool.scheduler())
                    | stdexec::then([this, jid = std::move(jid)]() mutable {
                          drain_step(jid);
                      });
        impl_->scope.spawn(std::move(sender));
    }

    void JidTaskRunner::drain_step(const std::string &jid) {
        std::unique_ptr<QueuedTask> task;
        {
            std::scoped_lock lock(mutex_);
            auto it = buckets_.find(jid);
            if (it == buckets_.end()) {
                return;
            }

            if (stopping_.load() && discard_pending_.load()) {
                for (auto &queued : it->second.tasks) {
                    queued->cancel();
                }
                it->second.tasks.clear();
                buckets_.erase(it);
                return;
            }

            if (it->second.tasks.empty()) {
                it->second.active = false;
                buckets_.erase(it);
                return;
            }

            task = std::move(it->second.tasks.front());
            it->second.tasks.pop_front();
        }

        task->execute();

        std::vector<std::unique_ptr<QueuedTask>> canceled_tasks;
        bool more = false;
        {
            std::scoped_lock lock(mutex_);
            auto it = buckets_.find(jid);
            if (it == buckets_.end()) {
                return;
            }

            if (discard_pending_.load()) {
                for (auto &queued : it->second.tasks) {
                    canceled_tasks.push_back(std::move(queued));
                }
                it->second.tasks.clear();
            }

            if (it->second.tasks.empty()) {
                it->second.active = false;
                buckets_.erase(it);
            } else {
                more = true;
            }
        }

        for (auto &pending_task : canceled_tasks) {
            pending_task->cancel();
        }

        if (more) {
            schedule_drain(jid);
        }
    }

    void JidTaskRunner::shutdown(bool discard_pending) {
        discard_pending_.store(discard_pending);
        if (stopping_.exchange(true)) {
            if (impl_ != nullptr) {
                static_cast<void>(stdexec::sync_wait(impl_->scope.on_empty()));
            }
            return;
        }

        std::vector<std::unique_ptr<QueuedTask>> canceled_tasks;
        {
            std::scoped_lock lock(mutex_);
            if (discard_pending) {
                for (auto &[jid, bucket] : buckets_) {
                    while (!bucket.tasks.empty()) {
                        canceled_tasks.push_back(std::move(bucket.tasks.front()));
                        bucket.tasks.pop_front();
                    }
                }
            }
        }

        for (auto &task : canceled_tasks) {
            task->cancel();
        }

        if (impl_ != nullptr) {
            if (discard_pending) {
                impl_->scope.request_stop();
            }
            static_cast<void>(stdexec::sync_wait(impl_->scope.on_empty()));
        }
    }

    JidTaskRunner::BlockingLease JidTaskRunner::acquire_blocking_lease() {
        return {};
    }

    std::size_t JidTaskRunner::worker_count() const {
        return worker_count_;
    }

} // namespace orangutan::channel
