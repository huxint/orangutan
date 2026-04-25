#include "automation/runtime.hpp"

#include "automation/driver.hpp"
#include "utils/transparent-lookup.hpp"

#include <expected>
#include <stdexcept>
#include <utility>

#include <spdlog/spdlog.h>
#include <stdexec/execution.hpp>

namespace orangutan::automation {

    namespace {

        struct LegacyAutomationPayload {
            std::string agent_key;
            std::string name;
            std::string prompt;
        };

        void from_json(const nlohmann::json &json, LegacyAutomationPayload &payload) {
            payload.agent_key = json.value("agent_key", "");
            payload.name = json.value("name", "");
            payload.prompt = json.value("prompt", "");
        }

        [[nodiscard]]
        auto decode_pipeline_step(const nlohmann::json &step_json) -> std::expected<ActionDescriptor, std::string> {
            if (!step_json.is_object()) {
                return std::unexpected("pipeline step must be an object");
            }
            const auto action_key = step_json.value("action_key", "");
            if (action_key.empty()) {
                return std::unexpected("pipeline step action key must not be blank");
            }
            const auto payload_it = step_json.find("payload");
            return ActionDescriptor{
                .action_key = action_key,
                .payload = payload_it != step_json.end() ? *payload_it : nlohmann::json::object(),
            };
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

        auto dispatch(const DispatchRequest &request, const ExecutionContext &context) -> ExecutorResult override {
            return runtime_.execute_dispatch(request, context);
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
      clock_(std::move(clock)) {
        auto registered = action_registry_.register_action<LegacyAutomationPayload>(
            "legacy.automation",
            [this](const LegacyAutomationPayload &payload, const ExecutionContext &) -> ExecutorResult {
                const auto automation = service_->find(payload.agent_key, payload.name);
                if (!automation.has_value()) {
                    return std::unexpected("automation not found for dispatch");
                }

                auto lease = acquire_agent_execution_lease(automation->agent_key);
                return service_->execute_outcome(*automation, current_time(), false).result;
            });
        if (!registered) {
            throw std::runtime_error("failed to register legacy automation action: " + registered.error());
        }
    }

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
        Driver *driver = nullptr;
        {
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
            driver = driver_.get();
        }

        service_->set_schedule_changed_callback([this] {
            Driver *active_driver = nullptr;
            {
                std::scoped_lock lock(scope_mutex_);
                active_driver = driver_.get();
            }
            if (active_driver != nullptr) {
                active_driver->wake();
            }
        });
        driver->start();
    }

    void AutomationRuntime::stop() {
        std::shared_ptr<exec::async_scope> scope;
        std::shared_ptr<std::atomic<bool>> stop_requested;
        Driver *driver = nullptr;
        service_->set_schedule_changed_callback({});
        {
            std::scoped_lock lock(scope_mutex_);
            scope = scope_;
            stop_requested = background_stop_requested_;
            driver = driver_.get();
            if (stop_requested != nullptr) {
                stop_requested->store(true);
            }
            if (running_.exchange(false)) {
                if (scope != nullptr) {
                    scope->request_stop();
                }
            }
        }

        if (driver != nullptr) {
            driver->stop();
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

    ActionRegistry &AutomationRuntime::actions() noexcept {
        return action_registry_;
    }

    const ActionRegistry &AutomationRuntime::actions() const noexcept {
        return action_registry_;
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

    auto AutomationRuntime::execute_action(const ActionDescriptor &action, const ExecutionContext &context) -> std::expected<ExecutionResult, std::string> {
        if (action.action_key == "pipeline") {
            const auto steps_it = action.payload.find("steps");
            if (steps_it == action.payload.end() || !steps_it->is_array() || steps_it->empty()) {
                return std::unexpected("pipeline steps must be a non-empty array");
            }

            auto combined = ExecutionResult{
                .success = true,
            };
            for (const auto &step_json : *steps_it) {
                auto step = decode_pipeline_step(step_json);
                if (!step) {
                    return std::unexpected(step.error());
                }
                auto result = execute_action(*step, context);
                if (!result) {
                    return std::unexpected(result.error());
                }
                combined = std::move(*result);
                if (!combined.success) {
                    return combined;
                }
            }
            return combined;
        }

        return action_registry_.dispatch(action, context);
    }

    auto AutomationRuntime::execute_dispatch(const DispatchRequest &request, const ExecutionContext &context) -> std::expected<ExecutionResult, std::string> {
        return execute_action(request.action, context);
    }

} // namespace orangutan::automation
