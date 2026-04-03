#include "channel/jid-task-runner.hpp"

#include <stdexec/execution.hpp>

#include <exception>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <type_traits>
#include <utility>

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

    JidTaskRunner::JidTaskRunner(std::size_t worker_count)
    : base_worker_count_(worker_count),
      desired_worker_count_(worker_count) {
        if (worker_count == 0) {
            throw std::invalid_argument("JidTaskRunner requires at least one worker");
        }

        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            spawn_worker_locked();
        }
    }

    JidTaskRunner::~JidTaskRunner() {
        shutdown(true);
    }

    void JidTaskRunner::submit(const std::string &jid, Task task) {
        if (jid.empty()) {
            throw std::invalid_argument("JidTaskRunner requires a non-empty jid");
        }

        if (!task) {
            spdlog::debug("Ignoring empty JidTaskRunner task for '{}'", jid);
            return;
        }

        auto pipeline = stdexec::schedule(SchedulerModel::Scheduler{.runner = this, .jid = jid}) | stdexec::then([task = std::move(task), jid]() mutable {
                            try {
                                task();
                            } catch (const std::exception &e) {
                                spdlog::error("Unhandled exception in JidTaskRunner task for '{}': {}", jid, e.what());
                            } catch (...) {
                                spdlog::error("Unhandled non-standard exception in JidTaskRunner task for '{}'", jid);
                            }
                        });
        stdexec::start_detached(std::move(pipeline));
    }

    void JidTaskRunner::enqueue_scheduled_task(const std::string &jid, std::unique_ptr<QueuedTask> task) {
        if (jid.empty()) {
            throw std::invalid_argument("JidTaskRunner requires a non-empty jid");
        }

        if (stopping_.load()) {
            task->cancel();
            return;
        }

        {
            std::scoped_lock lock(mutex_);
            if (stopping_.load()) {
                task->cancel();
                return;
            }

            auto &bucket = buckets_[jid];
            bucket.tasks.push_back(std::move(task));
            if (bucket.active) {
                return;
            }

            bucket.active = true;
            ready_jids_.push(jid);
        }

        cv_.notify_one();
    }

    void JidTaskRunner::shutdown(bool discard_pending) {
        std::vector<std::unique_ptr<QueuedTask>> canceled_tasks;
        {
            discard_pending_.store(discard_pending);
            stopping_.store(true);

            std::scoped_lock lock(mutex_);
            if (workers_.empty()) {
                return;
            }

            if (discard_pending_.load()) {
                ready_jids_ = {};
                for (auto it = buckets_.begin(); it != buckets_.end();) {
                    while (!it->second.tasks.empty()) {
                        canceled_tasks.push_back(std::move(it->second.tasks.front()));
                        it->second.tasks.pop_front();
                    }
                    if (!it->second.active) {
                        it = buckets_.erase(it);
                        continue;
                    }
                    ++it;
                }
            }
        }

        for (auto &task : canceled_tasks) {
            task->cancel();
        }

        cv_.notify_all();
        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    JidTaskRunner::BlockingLease JidTaskRunner::acquire_blocking_lease() {
        std::scoped_lock lock(mutex_);
        if (stopping_.load()) {
            return {};
        }

        ++desired_worker_count_;
        if (live_worker_count_ < desired_worker_count_) {
            spawn_worker_locked();
        }
        cv_.notify_one();
        return BlockingLease(this);
    }

    std::size_t JidTaskRunner::worker_count() const {
        return base_worker_count_;
    }

    void JidTaskRunner::spawn_worker_locked() {
        ++live_worker_count_;
        workers_.emplace_back([this] {
            worker_loop();
        });
    }

    void JidTaskRunner::release_blocking_lease() {
        std::scoped_lock lock(mutex_);
        if (desired_worker_count_ > base_worker_count_) {
            --desired_worker_count_;
        }
        cv_.notify_all();
    }

    void JidTaskRunner::worker_loop() {
        while (true) {
            std::string jid;
            std::unique_ptr<QueuedTask> task;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stopping_.load() || !ready_jids_.empty() || live_worker_count_ > desired_worker_count_;
                });

                if (ready_jids_.empty()) {
                    if (stopping_.load() || live_worker_count_ > desired_worker_count_) {
                        --live_worker_count_;
                        cv_.notify_all();
                        return;
                    }
                    continue;
                }

                jid = std::move(ready_jids_.front());
                ready_jids_.pop();

                auto it = buckets_.find(jid);
                if (it == buckets_.end() || it->second.tasks.empty()) {
                    continue;
                }

                task = std::move(it->second.tasks.front());
                it->second.tasks.pop_front();
            }

            task->execute();

            std::vector<std::unique_ptr<QueuedTask>> canceled_tasks;
            {
                std::scoped_lock lock(mutex_);
                auto it = buckets_.find(jid);
                if (it == buckets_.end()) {
                    continue;
                }

                if (discard_pending_.load()) {
                    while (!it->second.tasks.empty()) {
                        canceled_tasks.push_back(std::move(it->second.tasks.front()));
                        it->second.tasks.pop_front();
                    }
                }

                if (it->second.tasks.empty()) {
                    it->second.active = false;
                    buckets_.erase(it);
                    continue;
                }

                ready_jids_.push(jid);
                cv_.notify_one();
            }

            for (auto &pending_task : canceled_tasks) {
                pending_task->cancel();
            }
        }
    }

} // namespace orangutan::channel
