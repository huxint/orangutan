#include "bootstrap/agent-loop-worker.hpp"

#include "agent/agent-loop.hpp"
#include "bootstrap/runtime-assembler.hpp"
#include "orchestration/mailbox.hpp"
#include "tools/registry/tool-context.hpp"
#include "utils/escape.hpp"
#include "utils/format.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::bootstrap {

    namespace {

        std::string format_teammate_message_xml(const MailboxMessage &message) {
            return utils::format(R"(<teammate-message from="{}">{}</teammate-message>)", utils::escape_xml(message.from), utils::escape_xml(message.text));
        }

        /// `WorkerRuntime` backed by a full `AgentLoop` assembled via `build_agent_runtime`.
        /// Supports both fire-and-forget worker and persistent teammate lifecycles.
        class AgentLoopWorker final : public orchestration::WorkerRuntime {
        public:
            AgentLoopWorker(AgentRuntimeBundle bundle, std::string team_id, std::string agent_name, bool persistent)
            : bundle_(std::move(bundle)),
              team_id_(std::move(team_id)),
              agent_name_(std::move(agent_name)),
              persistent_(persistent) {}

            auto run(const std::string &prompt, std::stop_token stop_token) -> std::string override {
                bundle_.agent->set_stop_requested_callback([stop_token]() {
                    return stop_token.stop_requested();
                });
                attach_mailbox_fetcher();
                return bundle_.agent->run(prompt);
            }

            auto wait_for_next_prompt(std::stop_token stop_token) -> std::optional<std::string> override {
                if (!persistent_ || !has_mailbox_context()) {
                    return std::nullopt;
                }

                auto *mailbox = bundle_.tool_context().mailbox;
                while (!stop_token.stop_requested()) {
                    auto messages = mailbox->wait_for_messages(team_id_, agent_name_, std::chrono::seconds(30), stop_token);
                    if (messages.empty()) {
                        continue;
                    }
                    mailbox->mark_read({messages.front().id});
                    return std::move(messages.front().text);
                }
                return std::nullopt;
            }

            [[nodiscard]]
            auto is_persistent() const -> bool override {
                return persistent_;
            }

        private:
            [[nodiscard]]
            bool has_mailbox_context() const {
                return bundle_.tool_context().mailbox != nullptr && !team_id_.empty() && !agent_name_.empty();
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

                    std::vector<std::string> injected;
                    std::vector<std::string> ids;
                    injected.reserve(messages.size());
                    ids.reserve(messages.size());
                    for (const auto &message : messages) {
                        injected.push_back(format_teammate_message_xml(message));
                        ids.push_back(message.id);
                    }
                    mailbox->mark_read(ids);
                    return injected;
                });
            }

            AgentRuntimeBundle bundle_;
            std::string team_id_;
            std::string agent_name_;
            bool persistent_;
        };

    } // namespace

    orchestration::WorkerRuntimeFactory make_agent_loop_worker_factory(const Config &cfg,
                                                                        const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs,
                                                                        memory::MemoryStore *memory_store,
                                                                        orchestration::OrchestrationManager &orchestration_manager) {
        return [&cfg, &agent_runtime_configs, memory_store,
                &orchestration_manager](const orchestration::AgentSpawnRequest &request) -> std::unique_ptr<orchestration::WorkerRuntime> {
            const auto config_it = agent_runtime_configs.find(request.agent_key);
            if (config_it == agent_runtime_configs.end()) {
                throw std::runtime_error("No runtime configuration for agent '" + request.agent_key + "'.");
            }
            const auto &runtime_cfg = config_it->second;

            RuntimeIdentity identity{
                .workspace = runtime_cfg.workspace_root,
                .runtime_key = "agent:" + runtime_cfg.agent_key + "|worker:" + request.agent_name,
                .memory_scope = "agent:" + runtime_cfg.agent_key + "|worker",
            };

            auto bundle = build_agent_runtime(make_runtime_build_input(RuntimeAssemblyRequest{
                .runtime_config = &runtime_cfg,
                .identity = &identity,
                .app_config = &cfg,
                .memory_store = memory_store,
                .agent_name = request.agent_name,
                .team_agents = std::vector<std::string>{},
                .team_id = request.team_id,
                .orchestration_manager = &orchestration_manager,
                .agent_role = request.role,
                .delegated_task_prompt = request.task_prompt,
            }));

            return std::make_unique<AgentLoopWorker>(std::move(bundle), request.team_id, request.agent_name, orchestration::is_teammate(request.role));
        };
    }

} // namespace orangutan::bootstrap
