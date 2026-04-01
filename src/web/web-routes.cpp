#include "web/web-routes.hpp"
#include "web/web-route-internal.hpp"

#include "cli/cli-ui.hpp"
#include "cli/session-workflow.hpp"
#include "cli/single-shot.hpp"
#include "cli/slash-commands.hpp"
#include "automation/scheduler.hpp"
#include "bootstrap/bootstrap.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "providers/provider.hpp"
#include "tools/registry/permissions.hpp"
#include "agent/agent-loop.hpp"
#include "web/web-types.hpp"
#include "memory/memory-store.hpp"
#include "skills/skill-loader.hpp"
#include "subagent/subagent-manager.hpp"
#include "config/config.hpp"
#include "storage/session-store.hpp"
#include "tools/registry/tool.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <magic_enum/magic_enum.hpp>

namespace orangutan::web {

    namespace internal {

        namespace bootstrap = orangutan::bootstrap;
        namespace cli = orangutan::cli;

        bool session_is_read_only(const storage::SessionInfo &session) {
            return session.origin_kind == "channel";
        }

        storage::SessionMetadata make_web_session_metadata(const std::string &agent_key, const config::AgentConfig &agent) {
            return storage::SessionMetadata{
                .model = agent.model,
                .scope_key = "agent:" + agent_key + "|web",
                .agent_key = agent_key,
                .origin_kind = "web",
                .origin_ref = "web:local",
            };
        }

        std::optional<config::AgentConfig> find_effective_agent(const config::Config *config, const std::string &agent_key) {
            if (config == nullptr) {
                return std::nullopt;
            }

            const auto effective_agents = bootstrap::detail::build_effective_agents(*config);
            if (const auto it = effective_agents.find(agent_key); it != effective_agents.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        std::string resolve_agent_api_key(const config::Config &config, const config::AgentConfig &agent) {
            config::Config agent_cfg_wrapper = config;
            agent_cfg_wrapper.api_key = agent.api_key;
            return bootstrap::detail::resolve_api_key("", agent_cfg_wrapper);
        }

        std::string resolve_agent_workspace(const config::AgentConfig &agent, const std::string &agent_key) {
            try {
                return bootstrap::resolve_workspace_root(agent.workspace);
            } catch (const std::exception &e) {
                throw std::runtime_error("failed to resolve workspace for agent '" + agent_key + "': " + e.what());
            }
        }

        bootstrap::RuntimeIdentity derive_web_identity(const std::string &workspace_root, const std::string &agent_key) {
            bootstrap::RuntimeIdentity identity{
                .workspace = workspace_root,
                .runtime_key = "agent:" + agent_key + "|web:local",
                .memory_scope = "agent:" + agent_key + "|web",
            };
            return identity;
        }

        ToolApprovalCallback default_web_approval_callback() {
            return [](const ToolUse & /*call*/, const std::string & /*prompt_text*/) {
                // Task 3 keeps approval callback parity in runtime context.
                // Task 4 will replace this with request/response coordination.
                return false;
            };
        }

        bootstrap::AgentRuntimeBundle build_web_runtime_bundle_impl(const config::Config &config, const config::AgentConfig &agent, const std::string &agent_key,
                                                                    memory::MemoryStore *memory_store, std::string *current_session_id, subagent::SubagentManager *subagent_manager,
                                                                    automation::Runtime *automation_runtime, ToolApprovalCallback approval_callback,
                                                                    const std::shared_ptr<WebCompletionResumeState> &completion_resume_state) {
            const auto workspace_root = resolve_agent_workspace(agent, agent_key);
            const auto api_key = resolve_agent_api_key(config, agent);
            if (api_key.empty()) {
                throw providers::MissingApiKeyError("missing API key for agent '" + agent_key + "'");
            }

            auto effective_approval_callback = std::move(approval_callback);
            if (!effective_approval_callback) {
                effective_approval_callback = default_web_approval_callback();
            }

            bootstrap::AgentRuntimeBuildInput input{
                .provider_name = agent.provider,
                .api_key = api_key,
                .model = agent.model,
                .fallback_models = agent.fallback_models,
                .base_url = agent.base_url,
                .agent_key = agent_key,
                .system_prompt = agent.system_prompt,
                .workspace_root = workspace_root,
                .edit_mode = agent.edit_mode,
                .thinking_budget = agent.thinking_budget,
                .memory = config.memory,
                .permissions = agent.permissions,
                .allowed_child_agents = agent.subagents,
                .identity = derive_web_identity(workspace_root, agent_key),
                .memory_store = memory_store,
                .current_session_id = current_session_id,
                .subagent_manager = subagent_manager,
                .runtime_origin = base::origin::web,
                .raw_caller_id = "web:local",
                .automation_runtime = automation_runtime,
                .approval_callback = effective_approval_callback,
                .custom_tools = config.custom_tools,
                .mcp_servers = config.mcp_servers,
                .skill_paths = config.skill_paths,
                .hook_paths = config.hook_paths,
                .background_completion_runtime = (automation_runtime != nullptr && completion_resume_state != nullptr)
                                                     ? make_background_completion_runtime_bindings(
                                                           [automation_runtime](const automation::InboxItem &item) {
                                                               static_cast<void>(automation_runtime->store().insert_inbox(item));
                                                           },
                                                           detail::make_web_completion_resume_callback(completion_resume_state))
                                                     : nullptr,
            };
            return bootstrap::build_agent_runtime(input);
        }

        nlohmann::json session_to_json(const storage::SessionInfo &session) {
            return {
                {"id", session.id},
                {"created_at", session.created_at},
                {"model", session.model},
                {"scope_key", session.scope_key},
                {"agent_key", session.agent_key},
                {"origin_kind", session.origin_kind},
                {"origin_ref", session.origin_ref},
                {"message_count", session.message_count},
                {"read_only", session_is_read_only(session)},
            };
        }

        std::optional<storage::SessionInfo> find_agent_session(storage::SessionStore *store, const std::string &agent_key, const std::string &session_id) {
            if (store == nullptr) {
                return std::nullopt;
            }

            const auto sessions = store->list_sessions_for_agent(agent_key);
            for (const auto &session : sessions) {
                if (session.id == session_id) {
                    return session;
                }
            }
            return std::nullopt;
        }

        std::optional<std::string> extract_approval_command(const ToolUse &call) {
            if (!call.input.is_object()) {
                return std::nullopt;
            }
            const auto it = call.input.find("command");
            if (it == call.input.end() || !it->is_string()) {
                return std::nullopt;
            }
            return it->get<std::string>();
        }

        std::string make_approval_request_id() {
            static std::atomic<uint64_t> next_request_id{1};
            return "approval-" + std::to_string(next_request_id.fetch_add(1, std::memory_order_relaxed));
        }

        nlohmann::json approval_payload(const WebPendingApproval &approval) {
            nlohmann::json payload = {
                {"request_id", approval.request_id},
                {"tool", approval.tool},
                {"sandbox_mode", approval.sandbox_mode},
                {"prompt", approval.prompt},
            };
            if (approval.command.has_value()) {
                payload["command"] = *approval.command;
            }
            return payload;
        }

        std::string resolve_agent_key_param(const httplib::Request &req) {
            if (req.has_param("agent_key")) {
                return req.get_param_value("agent_key");
            }
            return "default";
        }

        nlohmann::json unix_time_to_json(const std::optional<base::i64> &value) {
            if (!value.has_value()) {
                return nullptr;
            }
            return *value;
        }

        nlohmann::json task_to_json(const automation::TaskSpec &task) {
            return {
                {"id", task.id},
                {"agent_key", task.agent_key},
                {"name", task.name},
                {"enabled", task.enabled},
                {"schedule_kind", magic_enum::enum_name(task.schedule.kind)},
                {"schedule", task.schedule.value},
                {"prompt", task.prompt},
                {"notes", task.notes},
                {"delivery", automation::delivery_policy_to_json(task.delivery)},
                {"last_run_at", unix_time_to_json(task.last_run_at)},
                {"last_status", task.last_status},
            };
        }

        nlohmann::json heartbeat_to_json(const automation::HeartbeatSpec &heartbeat) {
            return {
                {"id", heartbeat.id},
                {"agent_key", heartbeat.agent_key},
                {"name", heartbeat.name},
                {"enabled", heartbeat.enabled},
                {"paused", heartbeat.paused},
                {"every_seconds", heartbeat.every_seconds},
                {"jitter_seconds", heartbeat.jitter_seconds},
                {"active_hours", automation::active_hours_to_json(heartbeat.active_hours)},
                {"prompt", heartbeat.prompt},
                {"notes", heartbeat.notes},
                {"delivery", automation::delivery_policy_to_json(heartbeat.delivery)},
                {"next_due_at", unix_time_to_json(heartbeat.next_due_at)},
                {"last_run_at", unix_time_to_json(heartbeat.last_run_at)},
                {"last_status", heartbeat.last_status},
            };
        }

        nlohmann::json inbox_item_to_json(const automation::InboxItem &item) {
            return {
                {"id", item.id},         {"agent_key", item.agent_key}, {"source_kind", item.source_kind}, {"source_run_id", item.source_run_id},
                {"title", item.title},   {"body", item.body},           {"created_at", item.created_at},   {"acked_at", unix_time_to_json(item.acked_at)},
                {"status", item.status},
            };
        }

        void resolve_pending_approval(WebPendingApproval &approval, bool approved, bool cancelled) {
            std::scoped_lock lock(approval.mutex);
            if (approval.resolved) {
                return;
            }
            approval.resolved = true;
            approval.approved = approved;
            approval.cancelled = cancelled;
            approval.condition.notify_all();
        }

        void write_sse_event(httplib::DataSink &sink, std::string_view event_name, const nlohmann::json &payload) {
            const auto sse = "event: " + std::string(event_name) + "\ndata: " + payload.dump() + "\n\n";
            sink.write(sse.c_str(), sse.size());
        }

        void send_web_command_stream(httplib::Response &res, const cli::SlashCommandReply &command_response) {
            res.set_chunked_content_provider("text/event-stream", [command_response](std::size_t /*offset*/, httplib::DataSink &sink) -> bool {
                if (command_response.session_id.has_value()) {
                    write_sse_event(sink, "session", {{"session_id", *command_response.session_id}});
                }
                if (!command_response.text.empty()) {
                    write_sse_event(sink, "text", {{"text", command_response.text}});
                }
                write_sse_event(sink, "done", nlohmann::json::object());
                sink.done();
                return false;
            });
        }

        cli::SlashCommandReply handle_web_static_slash_command(const std::string &message, const std::string &agent_key, const config::Config &config, storage::SessionStore *store,
                                                               const std::optional<storage::SessionInfo> & /*existing_session*/, const storage::SessionMetadata &metadata,
                                                               const std::string &current_session_id) {
            return cli::dispatch_shared_slash_command(
                message, {.surface = cli::slash_command_surface::web,
                          .help =
                              [] {
                                  return cli::SlashCommandReply{.handled = true, .text = cli::web_help_text()};
                              },
                          .new_session =
                              [&] {
                                  std::string new_session_id;
                                  if (store != nullptr) {
                                      new_session_id = store->create_empty(metadata);
                                  } else {
                                      new_session_id = "web-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
                                  }
                                  return cli::SlashCommandReply{.handled = true, .text = {}, .session_id = new_session_id};
                              },
                          .export_session =
                              [&] {
                                  const auto maybe_agent = find_effective_agent(&config, agent_key);
                                  const auto workspace_root = maybe_agent.has_value() ? resolve_agent_workspace(*maybe_agent, agent_key) : std::string{};
                                  if (store == nullptr || current_session_id.empty()) {
                                      return cli::SlashCommandReply{
                                          .handled = true,
                                          .text = cli::describe_export_result(cli::export_session_markdown(std::vector<Message>{}, current_session_id, workspace_root)),
                                      };
                                  }
                                  return cli::SlashCommandReply{
                                      .handled = true,
                                      .text = cli::describe_export_result(cli::export_session_markdown(store->load(current_session_id), current_session_id, workspace_root)),
                                  };
                              },
                          .session =
                              [&] {
                                  return cli::SlashCommandReply{.handled = true, .text = cli::format_current_session(current_session_id, agent_key)};
                              },
                          .sessions =
                              [&] {
                                  if (store == nullptr) {
                                      return cli::SlashCommandReply{.handled = true, .text = "Session store is not available in this runtime."};
                                  }
                                  return cli::SlashCommandReply{
                                      .handled = true,
                                      .text = cli::format_scoped_sessions(store->list_sessions_for_agent(agent_key), current_session_id),
                                  };
                              },
                          .agent =
                              [&] {
                                  return cli::SlashCommandReply{.handled = true, .text = cli::format_current_agent(agent_key)};
                              },
                          .agents =
                              [&] {
                                  return cli::SlashCommandReply{.handled = true, .text = cli::format_agent_list(config, agent_key)};
                              },
                          .resume =
                              [&](const std::string &requested_session_id) {
                                  if (store == nullptr) {
                                      return cli::SlashCommandReply{.handled = true, .text = "Session store is not available in this runtime."};
                                  }
                                  const auto resolved_session_id = cli::resolve_requested_session(*store, requested_session_id, {}, agent_key);
                                  if (!resolved_session_id.has_value()) {
                                      return cli::SlashCommandReply{.handled = true, .text = "No saved sessions available for this agent."};
                                  }
                                  if (!store->session_belongs_to_agent(*resolved_session_id, agent_key)) {
                                      return cli::SlashCommandReply{.handled = true, .text = "That session does not belong to the current agent."};
                                  }
                                  return cli::SlashCommandReply{.handled = true, .text = "🧵 Resumed session: " + *resolved_session_id, .session_id = resolved_session_id};
                              }});
        }

        cli::SlashCommandReply handle_web_runtime_slash_command(const std::string &message, const std::string &agent_key, const config::AgentConfig &agent,
                                                                storage::SessionStore *store, const storage::SessionMetadata &metadata, bootstrap::AgentRuntimeBundle &runtime,
                                                                std::string &current_session_id) {
            return cli::dispatch_shared_slash_command(
                message, {
                             .surface = cli::slash_command_surface::web,
                             .compress =
                                 [&] {
                                     const auto result = runtime.agent->compress_history();
                                     if (result.compacted && store != nullptr && !current_session_id.empty()) {
                                         store->update(current_session_id, runtime.agent->history(), metadata);
                                     }
                                     return cli::SlashCommandReply{.handled = true, .text = cli::format_history_compaction_result(result)};
                                 },
                             .status =
                                 [&] {
                                     const auto active_model =
                                         runtime.provider != nullptr && !runtime.provider->current_model().empty() ? runtime.provider->current_model() : metadata.model;
                                     return cli::SlashCommandReply{
                                         .handled = true,
                                         .text = cli::format_runtime_status(cli::collect_runtime_status(*runtime.agent, *runtime.provider, &runtime.tools, current_session_id,
                                                                                                        agent_key, active_model, agent.fallback_models, metadata.scope_key)),
                                     };
                                 },
                             .tool_registry = &runtime.tools,
                         });
        }

    } // namespace internal

    bootstrap::AgentRuntimeBundle detail::build_web_runtime_bundle(const config::Config &config, const std::string &agent_key, memory::MemoryStore *memory_store,
                                                                   std::string *current_session_id, subagent::SubagentManager *subagent_manager,
                                                                   automation::Runtime *automation_runtime, ToolApprovalCallback approval_callback,
                                                                   const std::shared_ptr<WebCompletionResumeState> &completion_resume_state) {
        const auto maybe_agent = internal::find_effective_agent(&config, agent_key);
        if (!maybe_agent.has_value()) {
            throw std::runtime_error("agent not found");
        }
        return internal::build_web_runtime_bundle_impl(config, *maybe_agent, agent_key, memory_store, current_session_id, subagent_manager, automation_runtime,
                                                       std::move(approval_callback), completion_resume_state);
    }

    BackgroundCompletionResumeCallback detail::make_web_completion_resume_callback(const std::weak_ptr<WebCompletionResumeState> &weak_state) {
        return [weak_state](const std::string &message) -> std::optional<std::string> {
            const auto state = weak_state.lock();
            if (!state) {
                return "web session is no longer live";
            }

            std::scoped_lock lock(state->mutex);
            if (state->agent == nullptr) {
                return "web session is no longer live";
            }

            return orangutan::cli::run_completion_resume_message(*state->agent, message, state->agent_key, state->automation_runtime, {}, true);
        };
    }

    bool detail::await_web_approval(WebSessionState &session, std::mutex &sessions_mutex, const ToolUse &call, ToolSandboxMode sandbox_mode, const std::string &prompt_text,
                                    const web_approval_event_emitter &event_emitter, const std::function<bool()> &stream_open, std::chrono::milliseconds timeout) {
        auto approval = std::make_shared<WebPendingApproval>();
        approval->request_id = internal::make_approval_request_id();
        approval->tool = call.name;
        approval->command = internal::extract_approval_command(call);
        approval->sandbox_mode = to_string(sandbox_mode);
        approval->prompt = prompt_text;

        {
            std::scoped_lock sessions_lock(sessions_mutex);
            if (session.abort_requested.load()) {
                return false;
            }
            if (session.pending_approval != nullptr && !session.pending_approval->resolved) {
                return false;
            }
            session.pending_approval = approval;
        }

        if (event_emitter && !event_emitter("approval_request", internal::approval_payload(*approval))) {
            internal::resolve_pending_approval(*approval, false, true);
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock approval_lock(approval->mutex);
        while (!approval->resolved) {
            if (stream_open && !stream_open()) {
                approval->resolved = true;
                approval->approved = false;
                approval->cancelled = true;
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                approval->resolved = true;
                approval->approved = false;
                approval->cancelled = true;
                break;
            }

            const auto wait_for = std::min(std::chrono::milliseconds(100), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            approval->condition.wait_for(approval_lock, wait_for, [&approval] {
                return approval->resolved;
            });
        }
        const bool approved = approval->approved;
        approval_lock.unlock();

        {
            std::scoped_lock sessions_lock(sessions_mutex);
            if (session.pending_approval == approval) {
                session.pending_approval.reset();
            }
        }

        return approved;
    }

    void detail::cancel_pending_approval(WebSessionState &session) {
        if (session.pending_approval == nullptr) {
            return;
        }
        internal::resolve_pending_approval(*session.pending_approval, false, true);
    }
} // namespace orangutan::web
