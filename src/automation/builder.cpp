#include "automation/builder.hpp"

#include <cctype>
#include <stdexcept>

#include "automation/cron-parser.hpp"

namespace orangutan::automation {
    namespace {

        constexpr auto FULL_DAY_MINUTES = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::hours{24});

        [[nodiscard]]
        bool is_blank(std::string_view value) {
            for (const auto ch : value) {
                if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
                    return false;
                }
            }
            return true;
        }

        void validate_non_blank(std::string_view value, std::string_view label) {
            if (is_blank(value)) {
                throw std::invalid_argument(std::string(label) + " must not be blank");
            }
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

    AutomationBuilder &AutomationBuilder::for_agent(std::string_view agent_key) {
        automation_.agent_key = std::string(agent_key);
        return *this;
    }

    AutomationBuilder &AutomationBuilder::run_prompt(std::string_view prompt) {
        automation_.prompt = std::string(prompt);
        return *this;
    }

    AutomationBuilder &AutomationBuilder::with_notes(std::string_view notes) {
        automation_.notes = std::string(notes);
        return *this;
    }

    AutomationBuilder &AutomationBuilder::cron(std::string_view expression) {
        trigger_kind_ = trigger_type::cron;
        cron_expression_ = std::string(expression);
        every_ = std::chrono::seconds{0};
        jitter_ = std::chrono::seconds{0};
        active_windows_.clear();
        has_at_ = false;
        return *this;
    }

    AutomationBuilder &AutomationBuilder::every(std::chrono::seconds cadence) {
        trigger_kind_ = trigger_type::interval;
        cron_expression_.clear();
        every_ = cadence;
        has_at_ = false;
        return *this;
    }

    AutomationBuilder &AutomationBuilder::jitter(std::chrono::seconds amount) {
        jitter_ = amount;
        return *this;
    }

    AutomationBuilder &AutomationBuilder::once_at(TimePoint scheduled_at) {
        trigger_kind_ = trigger_type::once;
        cron_expression_.clear();
        every_ = std::chrono::seconds{0};
        jitter_ = std::chrono::seconds{0};
        active_windows_.clear();
        time_zone_ = "UTC";
        at_ = scheduled_at;
        has_at_ = true;
        return *this;
    }

    AutomationBuilder &AutomationBuilder::time_zone(std::string_view zone_name) {
        time_zone_ = std::string(zone_name);
        return *this;
    }

    AutomationBuilder &AutomationBuilder::within_hours(ActiveWindow window) {
        active_windows_.push_back(window);
        return *this;
    }

    AutomationBuilder &AutomationBuilder::deliver_to(std::string_view target) {
        automation_.delivery.mode = delivery_mode::notify;
        automation_.delivery.targets.push_back(std::string(target));
        return *this;
    }

    AutomationBuilder &AutomationBuilder::deliver_silently() {
        automation_.delivery.mode = delivery_mode::silent;
        automation_.delivery.targets.clear();
        return *this;
    }

    AutomationBuilder &AutomationBuilder::tag(std::string_view value) {
        automation_.tags.push_back(std::string(value));
        return *this;
    }

    AutomationBuilder &AutomationBuilder::enable() {
        automation_.enabled = true;
        automation_.paused = false;
        return *this;
    }

    AutomationBuilder &AutomationBuilder::disable() {
        automation_.enabled = false;
        automation_.paused = false;
        automation_.next_due_at.reset();
        return *this;
    }

    Automation AutomationBuilder::build() const {
        validate();

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

    void AutomationBuilder::validate() const {
        validate_non_blank(automation_.name, "automation name");
        validate_non_blank(automation_.agent_key, "agent key");
        validate_non_blank(automation_.prompt, "prompt");

        if (!trigger_kind_.has_value()) {
            throw std::invalid_argument("trigger must be configured");
        }

        if (!automation_.enabled && automation_.paused) {
            throw std::invalid_argument("disabled automations must not be paused");
        }

        if (automation_.delivery.mode == delivery_mode::notify && automation_.delivery.targets.empty()) {
            throw std::invalid_argument("notify delivery requires at least one target");
        }

        for (const auto &target : automation_.delivery.targets) {
            validate_non_blank(target, "delivery target");
        }

        for (const auto &tag : automation_.tags) {
            validate_non_blank(tag, "tag");
        }

        validate_non_blank(time_zone_, "time zone");
        if (const auto error = validate_time_zone_name(time_zone_); error.has_value()) {
            throw std::invalid_argument(*error);
        }

        switch (*trigger_kind_) {
        case trigger_type::cron:
            validate_non_blank(cron_expression_, "cron expression");
            if (!parse_cron_silent(cron_expression_).has_value()) {
                throw std::invalid_argument("cron expression is invalid");
            }
            if (every_ != std::chrono::seconds{0}) {
                throw std::invalid_argument("cron trigger does not accept interval cadence");
            }
            if (jitter_ != std::chrono::seconds{0}) {
                throw std::invalid_argument("cron trigger does not accept jitter");
            }
            if (!active_windows_.empty()) {
                throw std::invalid_argument("cron trigger does not accept active windows");
            }
            if (has_at_) {
                throw std::invalid_argument("cron trigger does not accept once timestamp");
            }
            break;
        case trigger_type::interval:
            if (every_ <= std::chrono::seconds{0}) {
                throw std::invalid_argument("interval cadence must be positive");
            }
            if (jitter_ < std::chrono::seconds{0}) {
                throw std::invalid_argument("interval jitter must not be negative");
            }
            if (!cron_expression_.empty()) {
                throw std::invalid_argument("interval trigger does not accept cron expression");
            }
            if (has_at_) {
                throw std::invalid_argument("interval trigger does not accept once timestamp");
            }
            for (const auto &window : active_windows_) {
                if (window.start < std::chrono::minutes{0} || window.end > FULL_DAY_MINUTES || window.start >= window.end) {
                    throw std::invalid_argument("active windows must be within one day and start before end");
                }
            }
            break;
        case trigger_type::once:
            if (!has_at_) {
                throw std::invalid_argument("once trigger requires a scheduled time");
            }
            if (!cron_expression_.empty()) {
                throw std::invalid_argument("once trigger does not accept cron expression");
            }
            if (every_ != std::chrono::seconds{0}) {
                throw std::invalid_argument("once trigger does not accept interval cadence");
            }
            if (jitter_ != std::chrono::seconds{0}) {
                throw std::invalid_argument("once trigger does not accept jitter");
            }
            if (!active_windows_.empty()) {
                throw std::invalid_argument("once trigger does not accept active windows");
            }
            if (time_zone_ != "UTC") {
                throw std::invalid_argument("once trigger only supports UTC time zone");
            }
            break;
        }
    }

} // namespace orangutan::automation
