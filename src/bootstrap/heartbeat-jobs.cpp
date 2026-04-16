#include "bootstrap/heartbeat-jobs.hpp"

#include "automation/builder.hpp"
#include "automation/category.hpp"
#include "automation/service.hpp"
#include "heartbeat/heartbeat-automation.hpp"
#include "utils/string.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap {
    namespace {

        [[nodiscard]]
        std::string resolve_heartbeat_channel(std::string_view channel) {
            auto target = utils::trim_copy(channel);
            if (target.empty()) {
                return "cli";
            }

            return std::string(target);
        }

        [[nodiscard]]
        std::string resolve_heartbeat_agent(std::string_view agent_key) {
            auto value = utils::trim_copy(agent_key);
            if (value.empty()) {
                return "default";
            }

            return std::string(value);
        }

        [[nodiscard]]
        std::optional<automation::Automation> build_heartbeat_automation(const Config::HeartbeatJobConfig &job) {
            auto result = automation::Automation::named(job.name)
                .for_agent(resolve_heartbeat_agent(job.agent))
                .run_prompt(job.prompt)
                .cron(job.cron)
                .deliver_to(resolve_heartbeat_channel(job.channel))
                .tag(heartbeat::HEARTBEAT_AUTOMATION_TAG)
                .tag(heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG)
                .build();

            if (!result.has_value()) {
                spdlog::warn("failed to build heartbeat automation '{}': {}", job.name, result.error());
                return std::nullopt;
            }
            return std::move(*result);
        }

        [[nodiscard]]
        std::string make_dedup_key(std::string_view agent_key, std::string_view name) {
            auto key = std::string(agent_key);
            key.push_back('\n');
            key.append(name);
            return key;
        }

    } // namespace

    void reconcile_heartbeat_jobs(const Config &cfg, automation::AutomationService &service) {
        std::vector<automation::Automation> desired;
        std::unordered_set<std::string> seen_keys;
        desired.reserve(cfg.heartbeat_jobs.size());

        for (const auto &job : cfg.heartbeat_jobs) {
            auto candidate = build_heartbeat_automation(job);
            if (!candidate.has_value()) {
                continue;
            }

            const auto key = make_dedup_key(candidate->agent_key, candidate->name);
            if (!seen_keys.insert(key).second) {
                spdlog::warn("heartbeat job '{}' for agent '{}' appears more than once; skipping duplicate entry", candidate->name, candidate->agent_key);
                continue;
            }

            desired.push_back(std::move(*candidate));
        }

        automation::reconcile_managed_automations(service, heartbeat::MANAGED_HEARTBEAT_AUTOMATION_TAG, desired);
    }

} // namespace orangutan::bootstrap
