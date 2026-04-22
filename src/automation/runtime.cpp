#include "automation/runtime.hpp"

#include "utils/sender-utils.hpp"

#include <chrono>
#include <utility>

#include <exec/repeat_effect_until.hpp>
#include <exec/timed_scheduler.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

namespace orangutan::automation {

    class AutomationRuntime::AgentExecutionGate {
    public:
        std::mutex mutex;
        std::condition_variable cv;
        std::thread::id owner;
        std::size_t depth = 0;
    };

    AutomationRuntime::AgentExecutionLease::AgentExecutionLease(std::shared_ptr<AgentExecutionGate> gate)
    : gate_(std::move(gate)),
      owner_(std::this_thread::get_id()) {
        std::unique_lock lock(gate_->mutex);
        gate_->cv.wait(lock, [this] {
            return gate_->depth == 0 || gate_->owner == owner_;
        });
        gate_->owner = owner_;
        ++gate_->depth;
    }

    AutomationRuntime::AgentExecutionLease::AgentExecutionLease(AgentExecutionLease &&other) noexcept
    : gate_(std::exchange(other.gate_, nullptr)),
      owner_(std::exchange(other.owner_, std::thread::id{})) {}

    AutomationRuntime::AgentExecutionLease &AutomationRuntime::AgentExecutionLease::operator=(AgentExecutionLease &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();
        gate_ = std::exchange(other.gate_, nullptr);
        owner_ = std::exchange(other.owner_, std::thread::id{});
        return *this;
    }

    AutomationRuntime::AgentExecutionLease::~AgentExecutionLease() {
        release();
    }

    void AutomationRuntime::AgentExecutionLease::release() noexcept {
        auto gate = std::exchange(gate_, nullptr);
        if (gate == nullptr) {
            return;
        }

        std::unique_lock lock(gate->mutex);
        if (gate->owner == owner_ && gate->depth > 0) {
            --gate->depth;
            if (gate->depth == 0) {
                gate->owner = std::thread::id{};
                lock.unlock();
                gate->cv.notify_all();
            }
        }
    }

    AutomationRuntime::AutomationRuntime(AutomationService &service, utils::TaskPool &pool, ClockSource clock)
    : service_(&service),
      pool_(&pool),
      clock_(std::move(clock)) {}

    AutomationRuntime::~AutomationRuntime() {
        stop();
    }

    void AutomationRuntime::set_executor(AutomationExecutor executor) {
        service_->set_executor(std::move(executor));
    }

    void AutomationRuntime::add_delivery_filter(AutomationDeliveryFilter filter) {
        service_->add_delivery_filter(std::move(filter));
    }

    void AutomationRuntime::register_category(AutomationCategory category) {
        service_->register_category(std::move(category));
    }

    void AutomationRuntime::set_notifier(AutomationNotifier notifier) {
        service_->set_notifier(std::move(notifier));
    }

    void AutomationRuntime::dispatch_background(std::function<void()> work) {
        if (!work) {
            return;
        }

        std::shared_ptr<exec::async_scope> scope;
        std::shared_ptr<std::atomic<bool>> stop_requested;
        {
            std::scoped_lock lock(scope_mutex_);
            if (!running_.load() || scope_ == nullptr || background_stop_requested_ == nullptr) {
                return;
            }
            scope = scope_;
            stop_requested = background_stop_requested_;
        }

        auto task = stdexec::schedule(pool_->scheduler())
                  | stdexec::then([job = std::move(work), stop_requested = std::move(stop_requested)]() mutable {
                        if (stop_requested != nullptr && stop_requested->load()) {
                            return;
                        }
                        try {
                            job();
                        } catch (const std::exception &error) {
                            spdlog::warn("background automation task failed: {}", error.what());
                        } catch (...) {
                            spdlog::warn("background automation task failed with unknown exception");
                        }
                    });
        scope->spawn(std::move(task));
    }

    void AutomationRuntime::start() {
        std::scoped_lock lock(scope_mutex_);
        if (running_.exchange(true)) {
            return;
        }
        if (scope_ == nullptr) {
            scope_ = std::make_shared<exec::async_scope>();
        }
        background_stop_requested_ = std::make_shared<std::atomic<bool>>(false);

        service_->normalize_state(current_time());

        // Drive the recurring wake-up from the timer context so the pool does not
        // keep a long-lived repeating task hot while automation is idle.
        auto tick = exec::schedule_after(pool_->timed_scheduler(), std::chrono::seconds{1})
                  | stdexec::let_value([this] {
                        return stdexec::schedule(pool_->scheduler())
                             | stdexec::then([this] {
                                   if (running_.load()) {
                                       try {
                                           run_pending(current_time());
                                       } catch (const std::exception &error) {
                                           spdlog::error("automation scheduler tick failed: {}", error.what());
                                       } catch (...) {
                                           spdlog::error("automation scheduler tick failed with unknown exception");
                                       }
                                   }
                                   return !running_.load();
                               });
                    })
                  | exec::repeat_effect_until();

        scope_->spawn(std::move(tick));
    }

    void AutomationRuntime::stop() {
        std::shared_ptr<exec::async_scope> scope;
        std::shared_ptr<std::atomic<bool>> stop_requested;
        {
            std::scoped_lock lock(scope_mutex_);
            scope = scope_;
            stop_requested = background_stop_requested_;
            if (scope == nullptr) {
                running_.store(false);
                background_stop_requested_.reset();
                return;
            }
            if (stop_requested != nullptr) {
                stop_requested->store(true);
            }
            if (running_.exchange(false)) {
                scope->request_stop();
            }
        }

        static_cast<void>(stdexec::sync_wait(scope->on_empty()));

        {
            std::scoped_lock lock(scope_mutex_);
            if (scope_ == scope) {
                scope_.reset();
            }
            if (background_stop_requested_ == stop_requested) {
                background_stop_requested_.reset();
            }
        }
    }

    void AutomationRuntime::run_pending(TimePoint now) {
        const auto due = service_->collect_due(now);
        auto pipeline = stdexec::just() | stdexec::then([this, due] {
                            for (const auto &entry : due) {
                                auto lease = acquire_agent_execution_lease(entry.automation.agent_key);
                                static_cast<void>(service_->execute(entry.automation, current_time()));
                            }
                        });
        static_cast<void>(execution::sync_wait_or_throw(std::move(pipeline), "automation runtime run_pending pipeline"));
    }

    AutomationRuntime::AgentExecutionLease AutomationRuntime::acquire_agent_execution_lease(std::string_view agent_key) {
        return AgentExecutionLease(get_agent_execution_gate(agent_key));
    }

    AutomationService &AutomationRuntime::service() noexcept {
        return *service_;
    }

    const AutomationService &AutomationRuntime::service() const noexcept {
        return *service_;
    }

    TimePoint AutomationRuntime::current_time() const {
        if (clock_ != nullptr) {
            return clock_();
        }

        return Clock::now();
    }

    std::shared_ptr<AutomationRuntime::AgentExecutionGate> AutomationRuntime::get_agent_execution_gate(std::string_view agent_key) {
        const std::string key = agent_key.empty() ? "default" : std::string(agent_key);
        std::scoped_lock lock(agent_execution_gates_mutex_);

        auto &entry = agent_execution_gates_[key];
        auto gate = entry.lock();
        if (gate == nullptr) {
            gate = std::make_shared<AgentExecutionGate>();
            entry = gate;
        }

        return gate;
    }

} // namespace orangutan::automation
