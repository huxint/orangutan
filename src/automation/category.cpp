#include "automation/category.hpp"

#include "automation/service.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace orangutan::automation {
    namespace {

        [[nodiscard]]
        std::string make_reconcile_key(std::string_view agent_key, std::string_view name) {
            auto key = std::string(agent_key);
            key.push_back('\n');
            key.append(name);
            return key;
        }

        void carry_runtime_state(const Automation &existing, Automation &desired) {
            desired.id = existing.id;
            desired.enabled = existing.enabled;
            desired.paused = existing.paused;
            desired.last_run_at = existing.last_run_at;
            desired.next_due_at = existing.next_due_at;
            desired.last_status = existing.last_status;
        }

    } // namespace

    bool has_tag(const Automation &automation, std::string_view tag) noexcept {
        return std::ranges::find(automation.tags, tag) != automation.tags.end();
    }

    void reconcile_managed_automations(AutomationService &service, std::string_view managed_tag, std::span<const Automation> desired) {
        const auto existing = service.list();

        std::unordered_map<std::string, const Automation *> managed_by_key;
        std::unordered_set<std::string> unmanaged_keys;
        for (const auto &entry : existing) {
            auto key = make_reconcile_key(entry.agent_key, entry.name);
            if (has_tag(entry, managed_tag)) {
                managed_by_key.emplace(std::move(key), &entry);
            } else {
                unmanaged_keys.insert(std::move(key));
            }
        }

        std::unordered_set<std::string> kept_managed_ids;
        for (const auto &desired_entry : desired) {
            const auto key = make_reconcile_key(desired_entry.agent_key, desired_entry.name);

            if (unmanaged_keys.contains(key) && !managed_by_key.contains(key)) {
                spdlog::warn("managed automation '{}' for agent '{}' conflicts with an existing unmanaged automation; skipping reconciliation", desired_entry.name,
                             desired_entry.agent_key);
                continue;
            }

            auto copy = desired_entry;
            if (const auto it = managed_by_key.find(key); it != managed_by_key.end()) {
                carry_runtime_state(*it->second, copy);
            }

            const auto saved_id = service.save(std::move(copy));
            if (!saved_id.empty()) {
                kept_managed_ids.insert(saved_id);
            }
        }

        for (const auto &entry : existing) {
            if (!has_tag(entry, managed_tag)) {
                continue;
            }
            if (kept_managed_ids.contains(entry.id)) {
                continue;
            }
            static_cast<void>(service.remove(entry.agent_key, entry.id));
        }
    }

} // namespace orangutan::automation
