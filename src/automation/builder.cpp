#include "automation/builder.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <ranges>

#include "automation/cron-parser.hpp"

namespace orangutan::automation {
    namespace {

        constexpr auto FULL_DAY_MINUTES = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours{24});

        [[nodiscard]]
        bool is_blank(std::string_view value) {
            return std::ranges::all_of(value, [](unsigned char ch) {
                return std::isspace(ch) != 0;
            });
        }

        [[nodiscard]]
        std::optional<std::string> validate_time_zone_name(std::string_view value) {
            if (value == "UTC") {
                return std::nullopt;
            }

            try {
                static_cast<void>(std::chrono::locate_zone(std::string(value)));
            } catch (const std::runtime_error &) {
                return std::string("time_zone must be UTC or a valid IANA zone name");
            }

            return std::nullopt;
        }

    } // namespace

    AutomationBuilder::AutomationBuilder(std::string_view name) {
        automation_.name = std::string(name);
        automation_.delivery.mode = delivery_mode::silent;
    }

    auto AutomationBuilder::build() const -> std::expected<Automation, std::string> {
        if (auto error = validate(); error.has_value()) {
            return std::unexpected(std::move(*error));
        }
        auto automation = automation_;
        if (automation.id.empty()) {
            automation.id = generate_id("auto");
        }
        automation.trigger = build_trigger();
        return automation;
    }

    TriggerDefinition AutomationBuilder::build_trigger() const {
        TriggerDefinition trigger;
        trigger.type = *trigger_kind_;

        switch (*trigger_kind_) {
        case trigger_type::cron:
            trigger.cron = cron_expression_;
            trigger.time_zone = time_zone_;
            break;
        case trigger_type::interval:
            trigger.every = every_;
            trigger.jitter = jitter_;
            trigger.time_zone = time_zone_;
            trigger.active_windows = active_windows_;
            break;
        case trigger_type::once:
            trigger.at = at_;
            trigger.time_zone = "UTC";
            break;
        }

        return trigger;
    }

    std::optional<std::string> AutomationBuilder::validate() const {
        if (is_blank(automation_.name)) { return "automation name must not be blank"; }
        if (is_blank(automation_.agent_key)) { return "agent key must not be blank"; }
        if (is_blank(automation_.prompt)) { return "prompt must not be blank"; }

        if (!trigger_kind_.has_value()) {
            return "trigger must be configured";
        }

        if (!automation_.enabled && automation_.paused) {
            return "disabled automations must not be paused";
        }

        if (automation_.delivery.mode == delivery_mode::notify && automation_.delivery.targets.empty()) {
            return "notify delivery requires at least one target";
        }

        if (!std::ranges::all_of(automation_.delivery.targets, [](std::string_view t) { return !is_blank(t); })) {
            return "delivery target must not be blank";
        }

        if (!std::ranges::all_of(automation_.tags, [](std::string_view t) { return !is_blank(t); })) {
            return "tag must not be blank";
        }

        if (is_blank(time_zone_)) { return "time zone must not be blank"; }
        if (const auto error = validate_time_zone_name(time_zone_); error.has_value()) {
            return *error;
        }

        switch (*trigger_kind_) {
        case trigger_type::cron:
            if (is_blank(cron_expression_)) { return "cron expression must not be blank"; }
            if (!parse_cron_silent(cron_expression_).has_value()) {
                return "cron expression is invalid";
            }
            if (every_ != std::chrono::seconds{0}) {
                return "cron trigger does not accept interval cadence";
            }
            if (jitter_ != std::chrono::seconds{0}) {
                return "cron trigger does not accept jitter";
            }
            if (!active_windows_.empty()) {
                return "cron trigger does not accept active windows";
            }
            if (has_at_) {
                return "cron trigger does not accept once timestamp";
            }
            break;
        case trigger_type::interval:
            if (every_ <= std::chrono::seconds{0}) {
                return "interval cadence must be positive";
            }
            if (jitter_ < std::chrono::seconds{0}) {
                return "interval jitter must not be negative";
            }
            if (!cron_expression_.empty()) {
                return "interval trigger does not accept cron expression";
            }
            if (has_at_) {
                return "interval trigger does not accept once timestamp";
            }
            for (const auto &window : active_windows_) {
                if (window.start < std::chrono::minutes{0} || window.end > FULL_DAY_MINUTES || window.start >= window.end) {
                    return "active windows must be within one day and start before end";
                }
            }
            break;
        case trigger_type::once:
            if (!has_at_) {
                return "once trigger requires a scheduled time";
            }
            if (!cron_expression_.empty()) {
                return "once trigger does not accept cron expression";
            }
            if (every_ != std::chrono::seconds{0}) {
                return "once trigger does not accept interval cadence";
            }
            if (jitter_ != std::chrono::seconds{0}) {
                return "once trigger does not accept jitter";
            }
            if (!active_windows_.empty()) {
                return "once trigger does not accept active windows";
            }
            if (time_zone_ != "UTC") {
                return "once trigger only supports UTC time zone";
            }
            break;
        }

        return std::nullopt;
    }

} // namespace orangutan::automation
