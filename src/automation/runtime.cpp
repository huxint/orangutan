#include "automation/runtime.hpp"

#include "utils/sender-utils.hpp"

#include <chrono>
#include <utility>

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

    AutomationRuntime::AutomationRuntime(AutomationService &service, ClockSource clock)
    : service_(&service),
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
        worker_ = std::jthread([this](std::stop_token stop_token) {
            scheduler_loop(stop_token);
        });
    }

    void AutomationRuntime::stop() {
        const bool was_running = running_.exchange(false);
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.request_stop();
            worker_.join();
        }

        if (!was_running) {
            return;
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

    void AutomationRuntime::scheduler_loop(std::stop_token stop_token) {
        while (running_.load() && !stop_token.stop_requested()) {
            run_pending(current_time());
            std::unique_lock lock(mutex_);
            static_cast<void>(cv_.wait_for(lock, stop_token, std::chrono::seconds{1}, [this] {
                return !running_.load();
            }));
        }
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
