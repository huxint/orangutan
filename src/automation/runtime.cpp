#include "automation/runtime.hpp"

#include "utils/sender-utils.hpp"

#include <chrono>
#include <utility>

#include <exec/repeat_effect_until.hpp>
#include <exec/timed_scheduler.hpp>
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
        if (!gate) {
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

    void AutomationRuntime::start() {
        if (running_.exchange(true)) {
            return;
        }

        service_->normalize_state(current_time());

        auto tick = stdexec::schedule(pool_->scheduler())
                  | stdexec::then([this] {
                        if (running_.load()) {
                            try {
                                run_pending(current_time());
                            } catch (...) { // NOLINT(bugprone-empty-catch): next tick retries
                            }
                        }
                    })
                  | stdexec::let_value([this] {
                        return exec::schedule_after(pool_->timed_scheduler(), std::chrono::seconds{1})
                             | stdexec::then([this] {
                                   return !running_.load();
                               });
                    })
                  | exec::repeat_effect_until();

        scope_.spawn(std::move(tick));
    }

    void AutomationRuntime::stop() {
        if (!running_.exchange(false)) {
            static_cast<void>(stdexec::sync_wait(scope_.on_empty()));
            return;
        }

        scope_.request_stop();
        static_cast<void>(stdexec::sync_wait(scope_.on_empty()));
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
        if (!gate) {
            gate = std::make_shared<AgentExecutionGate>();
            entry = gate;
        }

        return gate;
    }

} // namespace orangutan::automation
