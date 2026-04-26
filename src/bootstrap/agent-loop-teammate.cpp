#include "bootstrap/agent-loop-teammate.hpp"

#include "agent/agent-loop.hpp"
#include "bootstrap/config-builder.hpp"
#include "bootstrap/runtime-assembler.hpp"
#include "orchestration/mailbox.hpp"
#include "orchestration/team-manager.hpp"
#include "tools/registry/tool-context.hpp"
#include "utils/escape.hpp"

#include <fmt/format.h>

#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::bootstrap {

    namespace {

        std::string format_teammate_message_xml(const MailboxMessage &message) {
            return fmt::format(R"(<teammate-message from="{}">{}</teammate-message>)", utils::escape_xml(message.from), utils::escape_xml(message.text));
        }

        std::vector<std::string> render_teammate_message_xml(const std::vector<MailboxMessage> &messages) {
            std::vector<std::string> rendered;
            rendered.reserve(messages.size());
            for (const auto &message : messages) {
                rendered.push_back(format_teammate_message_xml(message));
            }
            return rendered;
        }

    } // namespace

    namespace detail {

        std::string format_teammate_messages_prompt(std::span<const orchestration::MailboxMessage> messages) {
            std::string prompt;
            for (const auto &message : messages) {
                if (!prompt.empty()) {
                    prompt += "\n";
                }
                prompt += format_teammate_message_xml(message);
            }
            return prompt;
        }

    } // namespace detail

    namespace {

        std::vector<std::string> collect_message_ids(const std::vector<MailboxMessage> &messages) {
            std::vector<std::string> ids;
            ids.reserve(messages.size());
            for (const auto &message : messages) {
                ids.push_back(message.id);
            }
            return ids;
        }

        void apply_spawn_overrides(const Config &cfg, AgentRuntimeConfig &runtime_cfg, const orchestration::AgentSpawnRequest &request) {
            if (request.profile_override.empty() && request.model_override.empty()) {
                if (request.thinking_budget_override > 0) {
                    runtime_cfg.thinking_budget = request.thinking_budget_override;
                }
                return;
            }

            const auto profile_name = request.profile_override.empty() ? runtime_cfg.provider_route.primary.profile_name : request.profile_override;
            const auto model_name = request.model_override.empty() ? runtime_cfg.provider_route.primary.model : request.model_override;
            const auto profile_it = cfg.profiles.find(profile_name);
            if (profile_it == cfg.profiles.end()) {
                throw std::runtime_error("Unknown profile for spawned agent: " + profile_name);
            }
            const auto &profile = profile_it->second;
            const auto model_it = profile.models.find(model_name);
            if (model_it == profile.models.end()) {
                throw std::runtime_error("Unknown model for spawned agent: " + profile_name + ":" + model_name);
            }

            runtime_cfg.model = model_name;
            runtime_cfg.fallback_models.clear();
            runtime_cfg.provider_route.fallbacks.clear();
            runtime_cfg.provider_route.primary = providers::ModelTarget{
                .profile_name = profile_name,
                .model = model_name,
                .base_url = profile.base_url,
                .api_key = detail::resolve_api_key(runtime_cfg.api_key_override, profile),
                .headers = profile.headers,
                .default_max_tokens = model_it->second.max_tokens,
                .provider = providers::parse_provider_kind(model_it->second.provider),
                .protocol = providers::parse_protocol_kind(model_it->second.protocol),
                .thinking = model_it->second.thinking,
            };
            if (request.thinking_budget_override > 0) {
                runtime_cfg.thinking_budget = request.thinking_budget_override;
            }
        }

        /// `TeammateRuntime` backed by a full `AgentLoop` assembled via `build_agent_runtime`.
        class AgentLoopTeammate final : public orchestration::TeammateRuntime {
        public:
            AgentLoopTeammate(AgentRuntimeBundle bundle, std::string team_id, std::string agent_name)
            : bundle_(std::move(bundle)),
              team_id_(std::move(team_id)),
              agent_name_(std::move(agent_name)) {}

            auto run(const std::string &prompt, std::stop_token stop_token) -> std::string override {
                bundle_.agent->set_stop_requested_callback([stop_token]() {
                    return stop_token.stop_requested();
                });
                attach_mailbox_fetcher();
                return bundle_.agent->run(prompt);
            }

            auto poll_next_prompt() -> std::optional<std::string> override {
                if (!has_mailbox_context()) {
                    return std::nullopt;
                }
                auto *mailbox = bundle_.tool_context().mailbox;
                auto messages = mailbox->poll(team_id_, agent_name_);
                if (messages.empty()) {
                    return std::nullopt;
                }
                return consume_messages(messages);
            }

            [[nodiscard]]
            auto can_receive_followups() const -> bool override {
                return has_mailbox_context();
            }

        private:
            [[nodiscard]]
            bool has_mailbox_context() const {
                return bundle_.tool_context().mailbox != nullptr && !team_id_.empty() && !agent_name_.empty();
            }

            std::string consume_messages(const std::vector<MailboxMessage> &messages) {
                auto prompt = detail::format_teammate_messages_prompt(messages);
                bundle_.tool_context().mailbox->mark_read(collect_message_ids(messages));
                return prompt;
            }

            void attach_mailbox_fetcher() {
                if (!has_mailbox_context()) {
                    return;
                }
                auto *mailbox = bundle_.tool_context().mailbox;
                bundle_.agent->set_incoming_message_fetcher([mailbox, team = team_id_, name = agent_name_]() {
                    auto messages = mailbox->poll(team, name);
                    if (messages.empty()) {
                        return std::vector<std::string>{};
                    }

                    auto injected = render_teammate_message_xml(messages);
                    mailbox->mark_read(collect_message_ids(messages));
                    return injected;
                });
            }

            AgentRuntimeBundle bundle_;
            std::string team_id_;
            std::string agent_name_;
        };

    } // namespace

    orchestration::TeammateRuntimeFactory make_agent_loop_teammate_factory(const Config &cfg,
                                                                        const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs,
                                                                        memory::MemoryStore *memory_store,
                                                                        orchestration::OrchestrationManager &orchestration_manager,
                                                                        orchestration::TeamManager *team_manager,
                                                                        orchestration::AgentMailbox *mailbox) {
        return [&cfg, &agent_runtime_configs, memory_store,
                &orchestration_manager, team_manager, mailbox](const orchestration::AgentSpawnRequest &request) -> std::unique_ptr<orchestration::TeammateRuntime> {
            const auto config_key = request.config_agent_key.empty() ? std::string{"default"} : request.config_agent_key;
            const auto config_it = agent_runtime_configs.find(config_key);
            if (config_it == agent_runtime_configs.end()) {
                throw std::runtime_error("No runtime configuration for parent agent '" + config_key + "'.");
            }
            auto runtime_cfg = config_it->second;
            apply_spawn_overrides(cfg, runtime_cfg, request);
            runtime_cfg.agent_key = request.name;

            RuntimeIdentity identity{
                .workspace = runtime_cfg.workspace_root,
                .runtime_key = "agent:" + config_key + "|teammate:" + request.name,
                .memory_scope = "agent:" + config_key + "|teammate",
            };

            auto bundle = build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
                .runtime_config = &runtime_cfg,
                .identity = &identity,
                .app_config = &cfg,
                .memory_store = memory_store,
                .agent_name = request.name,
                .team_id = request.team_id,
                .orchestration_manager = &orchestration_manager,
                .team_manager = team_manager,
                .mailbox = mailbox,
                .agent_role = orchestration::agent_role::teammate,
                .delegated_task_prompt = request.task,
            }));

            return std::make_unique<AgentLoopTeammate>(std::move(bundle), request.team_id, request.name);
        };
    }

} // namespace orangutan::bootstrap
