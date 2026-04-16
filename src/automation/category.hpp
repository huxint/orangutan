#pragma once

#include "automation/delivery.hpp"
#include "automation/model.hpp"

#include <span>
#include <string>
#include <string_view>

namespace orangutan::automation {

    class AutomationService;

    /// Plugin-style grouping for automations that share a tag and optional
    /// delivery specialization. Categories compose via
    /// `AutomationService::register_category`.
    struct AutomationCategory {
        std::string tag;
        std::string managed_tag;
        AutomationDeliveryFilter delivery_filter;
    };

    /// Returns true if the automation carries the given tag.
    [[nodiscard]]
    bool has_tag(const Automation &automation, std::string_view tag) noexcept;

    /// Reconciles the set of managed automations identified by `managed_tag`
    /// with the `desired` list. Runtime-only state on existing managed entries
    /// is carried over to the matching desired entry by (agent_key, name).
    /// Managed entries absent from `desired` are removed. Entries whose
    /// (agent_key, name) collide with an unmanaged user automation are
    /// skipped rather than clobbering the user's work.
    void reconcile_managed_automations(AutomationService &service, std::string_view managed_tag, std::span<const Automation> desired);

} // namespace orangutan::automation

namespace orangutan {

    using automation::AutomationCategory;
    using automation::reconcile_managed_automations;

} // namespace orangutan
