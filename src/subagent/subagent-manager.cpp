#include "subagent/subagent-manager.hpp"

#include "agent/agent-loop.hpp"
#include "memory/runtime-memory.hpp"
#include "tools/runtime-loader/runtime-loader.hpp"
#include "providers/provider.hpp"
#include "infra/storage/session-store.hpp"
#include "infra/execution/sender-utils.hpp"
#include "app/runtime/memory-context.hpp"
#include "tools/registry/tool.hpp"

#include <chrono>
#include <exception>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {

    namespace {

        constexpr auto run_id_prefix = "subagent-run-";

        bool is_terminal_record(const std::optional<SubagentRunRecord> &maybe_run) {
            return maybe_run.has_value() && (maybe_run->status == SubagentRunStatus::succeeded || maybe_run->status == SubagentRunStatus::failed ||
                                             maybe_run->status == SubagentRunStatus::timed_out || maybe_run->status == SubagentRunStatus::abandoned);
        }

        void persist_worker_result(SubagentRunStore &run_store, std::string_view run_id, const SubagentWorkerResult &result) {
            switch (result.status) {
                case SubagentRunStatus::succeeded:
                    run_store.mark_succeeded(std::string(run_id), result.final_summary, result.final_output);
                    break;
                case SubagentRunStatus::failed:
                    run_store.mark_failed(std::string(run_id), result.error);
                    break;
                case SubagentRunStatus::timed_out:
                    run_store.mark_timed_out(std::string(run_id), result.error);
                    break;
                case SubagentRunStatus::abandoned:
                case SubagentRunStatus::queued:
                case SubagentRunStatus::running:
                    throw std::runtime_error("worker returned invalid terminal status");
            }
        }

        struct RealChildExecutionContext {
            SubagentChildRuntimeConfig child_config;
            std::unique_ptr<Provider> provider;
            ToolRegistry child_tools;
            std::unique_ptr<std::string> current_session_id;
            ToolRuntimeContext tool_context;
            std::unique_ptr<RuntimeMemory> child_memory;
            RuntimeMemory *child_runtime_memory = nullptr;

            [[nodiscard]]
            std::string resolve_active_model() const {
                const auto current_model = provider->current_model();
                return current_model.empty() ? child_config.model : current_model;
            }
        };

    } // namespace

    SubagentManager::SubagentManager(SubagentRunStore &run_store, Worker worker)
    : run_store_(run_store),
      worker_(std::move(worker)) {}

    SubagentManager::SubagentManager(SubagentRunStore &run_store, SubagentExecutionEnvironment environment)
    : run_store_(run_store),
      worker_([this](const SubagentWorkerRequest &request) {
          return run_real_child(request);
      }),
      agent_configs_(environment.agent_configs),
      session_store_(environment.session_store),
      memory_store_(environment.memory_store),
      provider_factory_(std::move(environment.provider_factory)) {
        if (provider_factory_ == nullptr) {
            provider_factory_ = [](const SubagentChildRuntimeConfig &config) {
                return create_provider_with_fallbacks(config.provider_name, config.api_key, config.model, config.base_url, config.fallback_models);
            };
        }
    }

    SubagentManager::~SubagentManager() {
        try {
            shutdown();
        } catch (const std::exception &error) {
            spdlog::error("Failed to shut down subagent manager cleanly: {}", error.what());
        }
    }

    SubagentSpawnResult SubagentManager::spawn(const SubagentSpawnRequest &request) {
        std::scoped_lock lock(mutex_);

        if (shutting_down_) {
            return SubagentSpawnResult{
                .accepted = false,
                .error = "subagent manager is shutting down",
            };
        }
        if (request.caller.is_child_run) {
            return SubagentSpawnResult{
                .accepted = false,
                .error = "child runs cannot spawn subagents",
            };
        }
        if (!is_allowed_child_agent(request.caller, request.child_agent_key)) {
            return SubagentSpawnResult{
                .accepted = false,
                .error = "child agent is not allowed for this caller",
            };
        }

        auto child_session_id = request.child_session_id;
        auto child_scope_key = request.child_scope_key;
        auto child_identity = RuntimeIdentity{};
        if (uses_real_execution()) {
            const auto maybe_child_config = resolve_child_config(request.child_agent_key);
            if (!maybe_child_config.has_value()) {
                return SubagentSpawnResult{
                    .accepted = false,
                    .error = "unknown child agent: " + request.child_agent_key,
                };
            }

            child_identity = derive_child_identity(maybe_child_config->workspace_root, request.caller.raw_caller_id, request.child_agent_key);
            child_scope_key = child_identity.memory_scope;
            child_session_id = session_store_->create_empty(
                SessionMetadata{.model = maybe_child_config->model, .scope_key = child_scope_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        }
        if (child_session_id.empty() || child_scope_key.empty()) {
            return SubagentSpawnResult{
                .accepted = false,
                .error = "child session metadata is incomplete",
            };
        }

        auto run_id = make_run_id();
        run_store_.create_run(SubagentRunCreateParams{
            .run_id = run_id,
            .parent_runtime_key = request.caller.runtime_key,
            .parent_session_id = request.caller.session_id,
            .parent_agent_key = request.caller.agent_key,
            .child_session_id = child_session_id,
            .child_agent_key = request.child_agent_key,
            .child_scope_key = child_scope_key,
            .task_summary = request.task_summary,
        });

        auto state = std::make_shared<ActiveRunState>();
        auto worker_request = SubagentWorkerRequest{
            .run_id = run_id,
            .caller = request.caller,
            .child_agent_key = request.child_agent_key,
            .child_scope_key = child_scope_key,
            .child_session_id = child_session_id,
            .task_summary = request.task_summary,
            .child_identity = std::move(child_identity),
            .stop_token = state->stop_source.get_token(),
        };
        state->worker_thread = std::thread([this, state, worker_request]() mutable {
            run_worker(state, worker_request);
        });
        active_runs_.emplace(run_id, state);

        return SubagentSpawnResult{
            .accepted = true,
            .run_id = std::move(run_id),
        };
    }

    SubagentStatusResult SubagentManager::status(const SubagentStatusRequest &request) {
        auto maybe_run = run_store_.load_run(request.run_id);
        if (maybe_run.has_value() && !can_access_run(request.caller, *maybe_run)) {
            maybe_run.reset();
        }
        if (maybe_run.has_value() && is_terminal_status(maybe_run->status)) {
            std::shared_ptr<ActiveRunState> state;
            {
                std::scoped_lock lock(mutex_);
                const auto it = active_runs_.find(request.run_id);
                if (it != active_runs_.end()) {
                    state = it->second;
                }
            }
            if (state != nullptr && is_finished(state)) {
                cleanup_finished_run(request.run_id, state);
            }
        }

        return SubagentStatusResult{.run = maybe_run};
    }

    SubagentWaitResult SubagentManager::wait(const SubagentWaitRequest &request) {
        std::shared_ptr<ActiveRunState> state;
        {
            std::scoped_lock lock(mutex_);
            const auto it = active_runs_.find(request.run_id);
            if (it != active_runs_.end()) {
                state = it->second;
            }
        }

        if (state != nullptr) {
            std::unique_lock lock(state->mutex);
            if (!state->cv.wait_for(lock, request.timeout, [&state] {
                    return state->completed;
                })) {
                return SubagentWaitResult{.state = SubagentWaitState::timed_out};
            }
            lock.unlock();
            cleanup_finished_run(request.run_id, state);
        }

        auto maybe_run = run_store_.load_run(request.run_id);
        if (!maybe_run.has_value()) {
            return SubagentWaitResult{.state = SubagentWaitState::not_found};
        }
        if (!can_access_run(request.caller, *maybe_run)) {
            return SubagentWaitResult{.state = SubagentWaitState::not_found};
        }
        if (!is_terminal_status(maybe_run->status)) {
            return SubagentWaitResult{.state = SubagentWaitState::timed_out};
        }
        return SubagentWaitResult{
            .state = SubagentWaitState::completed,
            .run = std::move(maybe_run),
        };
    }

    void SubagentManager::abandon_stale_runs(const std::string &parent_runtime_key) {
        run_store_.mark_active_runs_abandoned_for_runtime(parent_runtime_key);
    }

    void SubagentManager::shutdown() {
        std::vector<std::pair<std::string, std::shared_ptr<ActiveRunState>>> active_runs;
        {
            std::scoped_lock lock(mutex_);
            if (shutting_down_ && active_runs_.empty()) {
                return;
            }
            shutting_down_ = true;
            active_runs.reserve(active_runs_.size());
            for (const auto &entry : active_runs_) {
                active_runs.emplace_back(entry.first, entry.second);
            }
        }

        for (const auto &[run_id, state] : active_runs) {
            if (!is_finished(state)) {
                {
                    std::scoped_lock lock(state->mutex);
                    state->abandoned = true;
                    state->completed = true;
                    state->stop_source.request_stop();
                }
                try {
                    run_store_.mark_abandoned(run_id);
                } catch (const std::exception &) {
                    if (!is_terminal_record(run_store_.load_run(run_id))) {
                        throw;
                    }
                }
                state->cv.notify_all();
            }
        }

        for (const auto &[run_id, state] : active_runs) {
            {
                std::unique_lock lock(state->mutex);
                while (!state->worker_exited && state->worker_thread.joinable()) {
                    state->cv.wait(lock);
                }
            }
            cleanup_finished_run(run_id, state);
        }
    }

    std::string SubagentManager::make_run_id() {
        const auto counter = next_run_id_++;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return std::string{run_id_prefix} + std::to_string(ticks) + "-" + std::to_string(counter);
    }

    bool SubagentManager::is_terminal_status(SubagentRunStatus status) {
        return status == SubagentRunStatus::succeeded || status == SubagentRunStatus::failed || status == SubagentRunStatus::timed_out || status == SubagentRunStatus::abandoned;
    }

    bool SubagentManager::is_allowed_child_agent(const SubagentCallerContext &caller, const std::string &child_agent_key) {
        return std::ranges::any_of(caller.allowed_child_agents, [&child_agent_key](const std::string &allowed_child_agent) {
            return allowed_child_agent == child_agent_key;
        });
    }

    bool SubagentManager::can_access_run(const SubagentCallerContext &caller, const SubagentRunRecord &run) {
        if (caller.runtime_key.empty()) {
            return true;
        }

        return caller.runtime_key == run.parent_runtime_key;
    }

    bool SubagentManager::uses_real_execution() const {
        return agent_configs_ != nullptr && session_store_ != nullptr;
    }

    std::optional<SubagentChildRuntimeConfig> SubagentManager::resolve_child_config(const std::string &child_agent_key) const {
        if (agent_configs_ == nullptr) {
            return std::nullopt;
        }

        const auto it = agent_configs_->find(child_agent_key);
        if (it == agent_configs_->end()) {
            return std::nullopt;
        }
        return it->second;
    }

    SubagentWorkerResult SubagentManager::run_real_child(const SubagentWorkerRequest &request) {
        auto pipeline = stdexec::just() | stdexec::then([this, &request]() {
                            const auto maybe_child_config = resolve_child_config(request.child_agent_key);
                            if (!maybe_child_config.has_value()) {
                                throw std::runtime_error("unknown child agent: " + request.child_agent_key);
                            }
                            if (provider_factory_ == nullptr) {
                                throw std::runtime_error("subagent provider factory is not configured");
                            }

                            auto context = RealChildExecutionContext{
                                .child_config = *maybe_child_config,
                                .provider = provider_factory_(*maybe_child_config),
                                .child_tools = ToolRegistry{},
                                .current_session_id = std::make_unique<std::string>(request.child_session_id),
                                .tool_context =
                                    ToolRuntimeContext{
                                        .runtime_key = request.child_identity.runtime_key,
                                        .agent_key = request.child_agent_key,
                                        .scope_key = request.child_scope_key,
                                        .current_session_id = nullptr,
                                        .allowed_child_agents = maybe_child_config->allowed_child_agents,
                                        .is_child_run = true,
                                        .subagent_manager = this,
                                        .runtime_origin = request.caller.runtime_origin,
                                        .raw_caller_id = request.caller.raw_caller_id,
                                        .approval_callback = request.caller.approval_callback,
                                    },
                            };
                            context.tool_context.current_session_id = context.current_session_id.get();
                            if (memory_store_ != nullptr) {
                                context.child_memory =
                                    std::make_unique<RuntimeMemory>(*memory_store_, make_runtime_memory_context(request.child_identity, context.child_config.memory));
                                context.child_runtime_memory = context.child_memory.get();
                            }
                            return context;
                        }) |
                        stdexec::then([this, &request](RealChildExecutionContext context) mutable {
                            static_cast<void>(register_runtime_tools(context.child_tools, context.child_runtime_memory, request.child_identity.workspace, &context.tool_context, {},
                                                                     {}, &context.child_config.permissions, request.caller.approval_callback, context.child_config.edit_mode));

                            auto child_prompt = append_subagent_prompt_guidance(context.child_config.system_prompt, context.child_config.allowed_child_agents, true);
                            AgentLoop child_agent(*context.provider, context.child_tools, child_prompt, context.child_runtime_memory);
                            child_agent.set_thinking_budget(context.child_config.thinking_budget);
                            const auto persist_history = [this, &request, &context](const std::vector<Message> &history) {
                                session_store_->update(request.child_session_id, history, context.resolve_active_model());
                            };

                            const auto final_output = child_agent.run(request.task_summary, {}, {}, persist_history);
                            session_store_->update(request.child_session_id, child_agent.history(), context.resolve_active_model());
                            return SubagentWorkerResult{
                                .status = SubagentRunStatus::succeeded,
                                .final_summary = final_output,
                                .final_output = final_output,
                            };
                        });

        auto [result] = execution::sync_wait_or_throw(pipeline, "subagent run_real_child pipeline");
        return result;
    }

    void SubagentManager::run_worker(const std::shared_ptr<ActiveRunState> &state, const SubagentWorkerRequest &request) {
        try {
            auto pipeline = stdexec::just() | stdexec::then([this, state, &request]() -> std::optional<SubagentWorkerResult> {
                                run_store_.mark_running(request.run_id);
                                if (should_abandon(state)) {
                                    return std::nullopt;
                                }
                                return worker_(request);
                            }) |
                            stdexec::then([this, state, &request](std::optional<SubagentWorkerResult> result) {
                                if (!result.has_value() || should_abandon(state)) {
                                    return;
                                }
                                persist_worker_result(run_store_, request.run_id, *result);
                            });
            static_cast<void>(execution::sync_wait_or_throw(std::move(pipeline), "subagent run_worker pipeline"));
        } catch (const std::exception &error) {
            if (!should_abandon(state)) {
                try {
                    run_store_.mark_failed(request.run_id, error.what());
                } catch (const std::exception &mark_error) {
                    spdlog::warn("Failed to persist subagent failure for '{}': {}", request.run_id, mark_error.what());
                }
            }
        }

        finish_state(state);
    }

    void SubagentManager::finish_state(const std::shared_ptr<ActiveRunState> &state) {
        {
            std::scoped_lock lock(state->mutex);
            state->worker_exited = true;
            state->completed = true;
        }
        state->cv.notify_all();
    }

    bool SubagentManager::should_abandon(const std::shared_ptr<ActiveRunState> &state) {
        std::scoped_lock lock(state->mutex);
        return state->abandoned;
    }

    bool SubagentManager::is_finished(const std::shared_ptr<ActiveRunState> &state) {
        std::scoped_lock lock(state->mutex);
        return state->completed;
    }

    void SubagentManager::cleanup_finished_run(const std::string &run_id, const std::shared_ptr<ActiveRunState> &state) {
        auto worker_thread = std::thread{};
        {
            std::scoped_lock lock(state->mutex);
            if (state->worker_exited && state->worker_thread.joinable()) {
                worker_thread = std::move(state->worker_thread);
            }
        }

        if (worker_thread.joinable()) {
            worker_thread.join();
        } else {
            return;
        }

        std::scoped_lock lock(mutex_);
        const auto it = active_runs_.find(run_id);
        if (it != active_runs_.end() && it->second == state) {
            active_runs_.erase(it);
        }
    }

} // namespace orangutan
