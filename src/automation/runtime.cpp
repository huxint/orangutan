#include "automation/runtime.hpp"

#include "utils/transparent-lookup.hpp"
#include "utils/scope-exit.hpp"

#include <chrono>
#include <utility>

#include <exec/timed_scheduler.hpp>
#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

namespace orangutan::automation {

    namespace {

        constexpr std::size_t BATCH_LIMIT = 16;

    } // namespace

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

        auto task = stdexec::schedule(pool_->scheduler()) | stdexec::then([job = std::move(work), stop_requested = std::move(stop_requested)]() mutable {
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
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(scope_mutex_);
            if (running_.exchange(true)) {
                return;
            }
            if (scope_ == nullptr) {
                scope_ = std::make_shared<exec::async_scope>();
            }
            background_stop_requested_ = std::make_shared<std::atomic<bool>>(false);
            cycle_active_.store(false);
            generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        }

        service_->set_schedule_changed_callback([this] {
            std::uint64_t generation = 0;
            {
                std::scoped_lock lock(scope_mutex_);
                if (!running_.load()) {
                    return;
                }
                generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            }
            spawn_cycle(std::chrono::steady_clock::duration::zero(), generation);
        });
        spawn_cycle(std::chrono::steady_clock::duration::zero(), generation);
    }

    void AutomationRuntime::stop() {
        std::shared_ptr<exec::async_scope> scope;
        std::shared_ptr<std::atomic<bool>> stop_requested;
        service_->set_schedule_changed_callback({});
        {
            std::scoped_lock lock(scope_mutex_);
            scope = scope_;
            stop_requested = background_stop_requested_;
            if (stop_requested != nullptr) {
                stop_requested->store(true);
            }
            if (running_.exchange(false)) {
                generation_.fetch_add(1, std::memory_order_acq_rel);
                if (scope != nullptr) {
                    scope->request_stop();
                }
            }
        }

        if (scope == nullptr) {
            std::scoped_lock lock(scope_mutex_);
            background_stop_requested_.reset();
            return;
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
        cycle_active_.store(false);
    }

    void AutomationRuntime::run_pending(TimePoint now) {
        for (const auto &entry : service_->collect_due(now, BATCH_LIMIT)) {
            auto lease = acquire_agent_execution_lease(entry.automation.agent_key);
            static_cast<void>(service_->execute(entry.automation, current_time(), false));
        }
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
        const std::string_view key = agent_key.empty() ? "default" : agent_key;
        std::scoped_lock lock(agent_execution_gates_mutex_);

        auto it = utils::transparent_find(agent_execution_gates_, key);
        if (it == agent_execution_gates_.end()) {
            auto gate = std::make_shared<AgentExecutionGate>();
            static_cast<void>(agent_execution_gates_.emplace(std::string{key}, gate));
            return gate;
        }

        auto gate = it->second.lock();
        if (gate == nullptr) {
            gate = std::make_shared<AgentExecutionGate>();
            it->second = gate;
        }

        return gate;
    }

    void AutomationRuntime::spawn_cycle(std::chrono::steady_clock::duration delay, std::uint64_t generation) {
        std::shared_ptr<exec::async_scope> scope;
        {
            std::scoped_lock lock(scope_mutex_);
            if (!running_.load() || scope_ == nullptr || generation != generation_.load(std::memory_order_acquire)) {
                return;
            }
            scope = scope_;
        }

        auto task = exec::schedule_after(pool_->timed_scheduler(), delay) | stdexec::let_value([this, generation] {
                        return stdexec::schedule(pool_->scheduler()) | stdexec::then([this, generation] {
                                   run_cycle(generation);
                               });
                    });
        scope->spawn(std::move(task));
    }

    void AutomationRuntime::run_cycle(std::uint64_t generation) {
        if (!running_.load() || generation != generation_.load(std::memory_order_acquire)) {
            return;
        }

        bool expected = false;
        if (!cycle_active_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        std::optional<std::chrono::steady_clock::duration> next_delay;
        const auto release_cycle = utils::scope_exit([this, generation, &next_delay] {
            cycle_active_.store(false, std::memory_order_release);
            const auto current_generation = generation_.load(std::memory_order_acquire);
            if (running_.load() && current_generation != generation) {
                spawn_cycle(std::chrono::steady_clock::duration::zero(), current_generation);
                return;
            }
            if (running_.load() && next_delay.has_value()) {
                spawn_cycle(*next_delay, generation);
            }
        });

        const auto now = current_time();
        next_delay = next_cycle_delay(now);
        if (!next_delay.has_value()) {
            return;
        }
        if (*next_delay > std::chrono::steady_clock::duration::zero()) {
            return;
        }

        run_pending(now);
        next_delay = next_cycle_delay(current_time());
    }

    std::optional<std::chrono::steady_clock::duration> AutomationRuntime::next_cycle_delay(TimePoint now) const {
        const auto next_wakeup = service_->next_wakeup(now);
        if (!next_wakeup.has_value()) {
            return std::nullopt;
        }

        auto delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(*next_wakeup - now);
        if (delay < std::chrono::steady_clock::duration::zero()) {
            delay = std::chrono::steady_clock::duration::zero();
        }
        return delay;
    }

} // namespace orangutan::automation
