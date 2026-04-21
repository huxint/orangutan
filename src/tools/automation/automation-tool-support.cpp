#include "tools/automation/automation-tool-support.hpp"

#include "utils/expected-combine.hpp"

#include <magic_enum/magic_enum.hpp>

namespace orangutan::builtin::detail {

    std::string resolve_agent_key(const ToolRuntimeContext &ctx) {
        return ctx.agent_key.empty() ? std::string{"default"} : ctx.agent_key;
    }

    std::string resolve_query_agent_key(const ToolRuntimeContext *ctx, const nlohmann::json &request) {
        if (const auto it = request.find("agent_key"); it != request.end() && it->is_string()) {
            return it->get<std::string>();
        }
        if (ctx != nullptr && !ctx->agent_key.empty()) {
            return ctx->agent_key;
        }
        return {};
    }

    std::string id_or_name(const nlohmann::json &request) {
        return request.value("id", request.value("name", ""));
    }

    nlohmann::json normalize_automation_op_input(const nlohmann::json &input) {
        auto routed_input = input;
        routed_input["op"] = input.value("op", "");
        return routed_input;
    }

    std::expected<automation::DeliveryPolicy, ParseError> parse_delivery_overlay(const nlohmann::json &input, const automation::DeliveryPolicy &base) {
        auto delivery = base;

        if (const auto it = input.find("delivery"); it != input.end()) {
            if (!it->is_object()) {
                return std::unexpected("invalid delivery configuration");
            }

            const auto mode_name = it->value("mode", std::string{magic_enum::enum_name(base.mode)});
            const auto mode = magic_enum::enum_cast<automation::delivery_mode>(mode_name);
            if (!mode.has_value()) {
                return std::unexpected("invalid delivery configuration");
            }
            delivery.mode = *mode;

            const auto targets = it->value("targets", nlohmann::json::array());
            if (!targets.is_array()) {
                return std::unexpected("invalid delivery configuration");
            }

            delivery.targets.clear();
            delivery.targets.reserve(targets.size());
            for (const auto &item : targets) {
                if (!item.is_string()) {
                    return std::unexpected("invalid delivery configuration");
                }
                delivery.targets.push_back(item.get<std::string>());
            }
            return delivery;
        }

        if (const auto it = input.find("delivery_mode"); it != input.end()) {
            if (!it->is_string()) {
                return std::unexpected("invalid delivery configuration");
            }
            const auto mode = magic_enum::enum_cast<automation::delivery_mode>(it->get_ref<const std::string &>());
            if (!mode.has_value()) {
                return std::unexpected("invalid delivery configuration");
            }
            delivery.mode = *mode;
        }

        if (const auto it = input.find("targets"); it != input.end()) {
            if (!it->is_array()) {
                return std::unexpected("invalid delivery configuration");
            }

            delivery.targets.clear();
            delivery.targets.reserve(it->size());
            for (const auto &item : *it) {
                if (!item.is_string()) {
                    return std::unexpected("invalid delivery configuration");
                }
                delivery.targets.push_back(item.get<std::string>());
            }
        }

        return delivery;
    }

    std::expected<automation::TriggerDefinition, ParseError> parse_trigger_value(const nlohmann::json &input) {
        const auto it = input.find("trigger");
        if (it == input.end()) {
            return std::unexpected("trigger is required");
        }
        if (!it->is_object()) {
            return std::unexpected("invalid trigger configuration");
        }

        const auto parsed = automation::trigger_from_json(*it);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        return *parsed;
    }

    std::expected<std::vector<std::string>, ParseError> parse_tags_value(const nlohmann::json &input, const std::vector<std::string> &base) {
        const auto it = input.find("tags");
        if (it == input.end()) {
            return base;
        }
        if (!it->is_array()) {
            return std::unexpected("invalid tags configuration");
        }

        std::vector<std::string> tags;
        tags.reserve(it->size());
        for (const auto &item : *it) {
            if (!item.is_string()) {
                return std::unexpected("invalid tags configuration");
            }
            tags.push_back(item.get<std::string>());
        }
        return tags;
    }

    std::expected<automation::Automation, ParseError> parse_create_request(const nlohmann::json &request, std::string_view agent_key) {
        automation::Automation automation;
        automation.id = request.value("id", "");
        automation.agent_key = std::string(agent_key);
        automation.name = request.value("name", "");
        automation.prompt = request.value("prompt", "");
        automation.notes = request.value("notes", "");
        automation.enabled = request.value("enabled", true);
        automation.paused = request.value("paused", false);

        auto parts = utils::all_ok(
            parse_tags_value(request, {}),
            parse_trigger_value(request),
            parse_delivery_overlay(request, automation.delivery));
        if (!parts.has_value()) {
            return std::unexpected(parts.error());
        }
        auto &[tags, trigger, delivery] = *parts;
        automation.tags = std::move(tags);
        automation.trigger = std::move(trigger);
        automation.delivery = std::move(delivery);

        return automation;
    }

    std::expected<automation::Automation, ParseError> apply_update_request(const nlohmann::json &request, const automation::Automation &base, std::string_view agent_key) {
        auto automation = base;
        automation.agent_key = std::string(agent_key);

        if (const auto it = request.find("name"); it != request.end() && it->is_string()) {
            automation.name = it->get<std::string>();
        }
        if (const auto it = request.find("prompt"); it != request.end() && it->is_string()) {
            automation.prompt = it->get<std::string>();
        }
        if (const auto it = request.find("notes"); it != request.end() && it->is_string()) {
            automation.notes = it->get<std::string>();
        }
        if (const auto it = request.find("enabled"); it != request.end() && it->is_boolean()) {
            automation.enabled = it->get<bool>();
        }
        if (const auto it = request.find("paused"); it != request.end() && it->is_boolean()) {
            automation.paused = it->get<bool>();
        }

        auto parts = utils::all_ok(
            parse_tags_value(request, automation.tags),
            parse_delivery_overlay(request, automation.delivery));
        if (!parts.has_value()) {
            return std::unexpected(parts.error());
        }
        auto &[tags, delivery] = *parts;

        if (request.contains("trigger")) {
            const auto trigger = parse_trigger_value(request);
            if (!trigger.has_value()) {
                return std::unexpected(trigger.error());
            }
            automation.trigger = *trigger;
        }

        automation.tags = std::move(tags);
        automation.delivery = std::move(delivery);

        return automation;
    }

} // namespace orangutan::builtin::detail
