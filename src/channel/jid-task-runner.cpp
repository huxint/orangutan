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

    JidTaskRunner::BlockingLease::BlockingLease(JidTaskRunner *runner)
    : runner_(runner) {}

    JidTaskRunner::BlockingLease::~BlockingLease() {
        if (runner_ != nullptr) {
            runner_->release_blocking_lease();
        }
    }

    JidTaskRunner::BlockingLease::BlockingLease(BlockingLease &&other) noexcept
    : runner_(std::exchange(other.runner_, nullptr)) {}

    JidTaskRunner::BlockingLease &JidTaskRunner::BlockingLease::operator=(BlockingLease &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        if (runner_ != nullptr) {
            runner_->release_blocking_lease();
        }

        runner_ = std::exchange(other.runner_, nullptr);
        return *this;
    }

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
        /// One single-thread pool per active drainer slot. Pools are created
        /// lazily and kept for the runner lifetime, avoiding idle work-stealing
        /// between multiple workers while preserving per-JID parallelism.
        std::vector<std::unique_ptr<utils::TaskPool>> drain_pools;
        exec::async_scope scope;

        explicit Impl(std::size_t pool_size)
        : drain_pools{} {
            drain_pools.reserve(pool_size);
        }
    };

    namespace {

        constexpr std::size_t MIN_POOL_SIZE = 2;

        [[nodiscard]]
        std::size_t resolve_pool_size(std::size_t worker_count) {
            return std::max<std::size_t>(worker_count, MIN_POOL_SIZE);
        }

        [[nodiscard]]
        std::vector<std::size_t> reserve_drain_slots(std::size_t active_jid_count, std::size_t base_worker_count, std::size_t blocking_leases,
                                                     std::size_t &active_drainers) {
            std::vector<std::size_t> slots;
            const auto max_drainers = base_worker_count + blocking_leases;
            const auto desired_drainers = std::min(active_jid_count, max_drainers);
            while (active_drainers < desired_drainers) {
                slots.push_back(active_drainers);
                ++active_drainers;
            }
            return slots;
        }

    } // namespace

    JidTaskRunner::JidTaskRunner(std::size_t worker_count)
    : base_worker_count_(resolve_pool_size(worker_count)),
      worker_count_(worker_count),
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

        std::vector<std::size_t> slots;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_.load()) {
                task->cancel();
                return;
            }

            auto [it, inserted] = buckets_.try_emplace(std::string(jid));
            auto &bucket = it->second;
            bucket.tasks.push_back(std::move(task));
            if (!inserted) {
                return;
            }

            ready_jids_.emplace(jid);
            slots = reserve_drain_slots(buckets_.size(), base_worker_count_, blocking_leases_, active_drainers_);
        }

        for (const auto slot : slots) {
            schedule_drain_slot(slot);
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
                ready_jids_ = {};
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
        std::vector<std::size_t> slots;
        {
            std::scoped_lock lock(mutex_);
            if (stopping_.load()) {
                return {};
            }

            ++blocking_leases_;
            slots = reserve_drain_slots(buckets_.size(), base_worker_count_, blocking_leases_, active_drainers_);
        }

        for (const auto slot : slots) {
            schedule_drain_slot(slot);
        }
        return BlockingLease(this);
    }

    void JidTaskRunner::release_blocking_lease() {
        std::scoped_lock lock(mutex_);
        if (blocking_leases_ > 0) {
            --blocking_leases_;
        }
    }

    void JidTaskRunner::schedule_drain_slot(std::size_t slot_index) {
        utils::TaskPool *pool = nullptr;
        {
            std::scoped_lock lock(mutex_);
            while (impl_->drain_pools.size() <= slot_index) {
                impl_->drain_pools.push_back(std::make_unique<utils::TaskPool>(1));
            }
            pool = impl_->drain_pools[slot_index].get();
        }

        auto sender = stdexec::schedule(pool->scheduler()) | stdexec::then([this] {
                          drain_ready_tasks();
                      });
        impl_->scope.spawn(std::move(sender));
    }

    void JidTaskRunner::drain_ready_tasks() {
        while (true) {
            std::string jid;
            std::unique_ptr<QueuedTask> task;

            {
                std::scoped_lock lock(mutex_);
                if (ready_jids_.empty()) {
                    if (active_drainers_ > 0) {
                        --active_drainers_;
                    }
                    return;
                }

                jid = std::move(ready_jids_.front());
                ready_jids_.pop();

                auto it = buckets_.find(jid);
                if (it == buckets_.end()) {
                    continue;
                }
                if (it->second.tasks.empty()) {
                    buckets_.erase(it);
                    continue;
                }

                task = std::move(it->second.tasks.front());
                it->second.tasks.pop_front();
            }

            task->execute();

            std::vector<std::unique_ptr<QueuedTask>> canceled_tasks;
            std::vector<std::size_t> slots;
            {
                std::scoped_lock lock(mutex_);
                auto it = buckets_.find(jid);
                if (it != buckets_.end()) {
                    if (discard_pending_.load()) {
                        while (!it->second.tasks.empty()) {
                            canceled_tasks.push_back(std::move(it->second.tasks.front()));
                            it->second.tasks.pop_front();
                        }
                    }

                    if (it->second.tasks.empty()) {
                        buckets_.erase(it);
                    } else {
                        ready_jids_.push(jid);
                    }
                }

                if (!(stopping_.load() && discard_pending_.load())) {
                    slots = reserve_drain_slots(buckets_.size(), base_worker_count_, blocking_leases_, active_drainers_);
                }
            }

            for (auto &pending_task : canceled_tasks) {
                pending_task->cancel();
            }

            for (const auto slot : slots) {
                schedule_drain_slot(slot);
            }
        }
    }

    std::size_t JidTaskRunner::worker_count() const {
        return worker_count_;
    }

} // namespace orangutan::channel
