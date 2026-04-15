#include "bootstrap/heartbeat-jobs.hpp"

#include "automation/builder.hpp"
#include "automation/service.hpp"
#include "heartbeat/heartbeat-automation.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap {
    namespace {

        [[nodiscard]]
        std::string make_heartbeat_key(std::string_view agent_key, std::string_view name) {
            auto key = std::string(agent_key);
            key.push_back('\n');
            key.append(name);
            return key;
        }

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
        automation::Automation build_heartbeat_automation(const Config::HeartbeatJobConfig &job) {
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
                return {};
            }
            return std::move(*result);
        }

        void carry_runtime_state(const automation::Automation &existing, automation::Automation &desired) {
            desired.id = existing.id;
            desired.enabled = existing.enabled;
            desired.paused = existing.paused;
            desired.last_run_at = existing.last_run_at;
            desired.next_due_at = existing.next_due_at;
            desired.last_status = existing.last_status;
        }

    } // namespace

    void reconcile_heartbeat_jobs(const Config &cfg, automation::AutomationService &service) {
        const auto existing = service.list();

        std::unordered_map<std::string, std::vector<automation::Automation>> managed_by_key;
        for (const auto &automation : existing) {
            if (!heartbeat::is_managed_heartbeat_automation(automation)) {
                continue;
            }

            managed_by_key[make_heartbeat_key(automation.agent_key, automation.name)].push_back(automation);
        }

        std::unordered_set<std::string> desired_keys;
        std::unordered_set<std::string> kept_managed_ids;
        for (const auto &job : cfg.heartbeat_jobs) {
            auto desired = build_heartbeat_automation(job);
            const auto key = make_heartbeat_key(desired.agent_key, desired.name);
            if (desired_keys.contains(key)) {
                spdlog::warn("heartbeat job '{}' for agent '{}' appears more than once; skipping duplicate entry", desired.name, desired.agent_key);
                continue;
            }
            desired_keys.insert(key);

            const auto conflicting_unmanaged = std::ranges::any_of(existing, [&](const automation::Automation &automation) {
                if (make_heartbeat_key(automation.agent_key, automation.name) != key) {
                    return false;
                }

                return !heartbeat::is_managed_heartbeat_automation(automation);
            });
            if (conflicting_unmanaged && !managed_by_key.contains(key)) {
                spdlog::warn("heartbeat job '{}' for agent '{}' conflicts with an existing automation name; skipping managed heartbeat job reconciliation", desired.name,
                             desired.agent_key);
                continue;
            }

            if (const auto existing_it = managed_by_key.find(key); existing_it != managed_by_key.end() && !existing_it->second.empty()) {
                carry_runtime_state(existing_it->second.front(), desired);
            }

            const auto saved_id = service.save(std::move(desired));
            if (!saved_id.empty()) {
                kept_managed_ids.insert(saved_id);
            }
        }

        for (const auto &automation : existing) {
            if (!heartbeat::is_managed_heartbeat_automation(automation)) {
                continue;
            }
            if (kept_managed_ids.contains(automation.id)) {
                continue;
            }

            static_cast<void>(service.remove(automation.agent_key, automation.id));
        }
    }

} // namespace orangutan::bootstrap
