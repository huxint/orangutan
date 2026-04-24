#include "automation/runtime.hpp"

#include "automation/driver.hpp"
#include "automation/kernel.hpp"
#include "utils/transparent-lookup.hpp"

#include <chrono>
#include <expected>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

namespace orangutan::automation {

    namespace {

        [[nodiscard]]
        auto dispatch_agent_key(const DispatchRequest &request) -> std::string {
            if (const auto it = request.action.payload.find("agent_key"); it != request.action.payload.end() && it->is_string()) {
                return it->get<std::string>();
            }
            return {};
        }

    } // namespace

    class AutomationRuntime::AgentExecutionGate {
    public:
        std::mutex mutex;
        std::condition_variable cv;
        std::thread::id owner;
        std::size_t depth = 0;
    };

    class AutomationRuntime::RuntimeExecutorPort final : public ExecutorPort {
    public:
        explicit RuntimeExecutorPort(AutomationRuntime &runtime)
        : runtime_(runtime) {}

        auto dispatch(const DispatchRequest &request, const ExecutionContext &) -> ExecutorResult override {
            return runtime_.execute_dispatch(request);
        }

    private:
        AutomationRuntime &runtime_;
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

        if (driver_executor_ == nullptr) {
            driver_executor_ = std::make_unique<RuntimeExecutorPort>(*this);
        }
        if (driver_ == nullptr) {
            driver_ = std::make_unique<Driver>(service_->core_kernel(), *driver_executor_, *pool_, "legacy-runtime", [this] {
                return current_time();
            });
        }
        driver_->start();
    }

    void AutomationRuntime::stop() {
        std::shared_ptr<exec::async_scope> scope;
        std::shared_ptr<std::atomic<bool>> stop_requested;
        {
            std::scoped_lock lock(scope_mutex_);
            scope = scope_;
            stop_requested = background_stop_requested_;
            if (driver_ != nullptr) {
                driver_->stop();
            }
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
        auto due = service_->core_kernel().reserve_due(now, 128, "legacy-runtime-manual");
        if (!due) {
            throw std::runtime_error(due.error());
        }

        for (const auto &request : *due) {
            auto executed = run_request(request, now);
            if (!executed) {
                throw std::runtime_error(executed.error());
            }
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

    auto AutomationRuntime::execute_dispatch(const DispatchRequest &request) -> std::expected<ExecutionResult, std::string> {
        const auto automation = service_->find(dispatch_agent_key(request), request.job_id.value);
        if (!automation.has_value()) {
            return std::unexpected("automation not found for dispatch");
        }

        auto lease = acquire_agent_execution_lease(automation->agent_key);
        return service_->execute_outcome(*automation, current_time(), false).result;
    }

    auto AutomationRuntime::run_request(const DispatchRequest &request, TimePoint now) -> std::expected<void, std::string> {
        auto started = service_->core_kernel().mark_started(request.execution_id, now);
        if (!started) {
            return std::unexpected(started.error());
        }

        auto result = execute_dispatch(request);
        auto final_result = result.has_value()
            ? std::move(*result)
            : ExecutionResult{
                  .success = false,
                  .summary = result.error(),
              };

        auto finished = service_->core_kernel().mark_finished(request.execution_id, final_result, current_time());
        if (!finished) {
            return std::unexpected(finished.error());
        }
        if (!result) {
            return std::unexpected(result.error());
        }

        return {};
    }

} // namespace orangutan::automation
