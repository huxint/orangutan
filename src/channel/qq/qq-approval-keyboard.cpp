#include "channel/qq/qq-approval-keyboard.hpp"

#include <cctype>
#include <magic_enum/magic_enum.hpp>

namespace orangutan::channel::qq {

    namespace {

        struct ApprovalButtonSpec {
            std::string_view label;
            std::string_view visited_label;
            int style = 0;
        };

        [[nodiscard]]
        std::string trim_ascii(std::string_view value) {
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
                value.remove_prefix(1);
            }
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
                value.remove_suffix(1);
            }
            return std::string(value);
        }

        [[nodiscard]]
        ApprovalButtonSpec approval_button_spec_for(approval_action action) {
            switch (action) {
                case approval_action::allow_once:
                    return ApprovalButtonSpec{
                        .label = "Allow once",
                        .visited_label = "Approved",
                        .style = 1,
                    };
                case approval_action::always_allow:
                    return ApprovalButtonSpec{
                        .label = "Always allow",
                        .visited_label = "Always allowed",
                        .style = 1,
                    };
                case approval_action::deny:
                    return ApprovalButtonSpec{
                        .label = "Deny",
                        .visited_label = "Denied",
                        .style = 0,
                    };
            }
            return {};
        }

        [[nodiscard]]
        nlohmann::json build_approval_button(std::size_t index, std::string_view request_id, approval_action action) {
            const auto spec = approval_button_spec_for(action);
            return nlohmann::json{
                {"id", std::to_string(index)},
                {"render_data",
                 {
                     {"label", spec.label},
                     {"visited_label", spec.visited_label},
                     {"style", spec.style},
                 }},
                {"action",
                 {
                     // QQ inline keyboard must use callback buttons here; command buttons degrade to plain text input.
                     {"type", 1},
                     {"permission", {{"type", 2}}},
                     {"click_limit", 1},
                     {"data", build_approval_callback_data(request_id, action)},
                 }},
                {"group_id", "approval"},
            };
        }

    } // namespace

    std::string build_approval_callback_data(std::string_view request_id, approval_action action) {
        return "approval:" + std::string(request_id) + ":" + std::string(magic_enum::enum_name(action));
    }

    std::optional<ParsedApprovalCallbackData> parse_approval_callback_data(std::string_view data) {
        constexpr std::string_view PREFIX = "approval:";
        const auto trimmed = trim_ascii(data);
        if (!trimmed.starts_with(PREFIX)) {
            return std::nullopt;
        }

        const auto request_start = PREFIX.size();
        const auto action_separator = trimmed.find(':', request_start);
        if (action_separator == std::string::npos || action_separator == request_start) {
            return std::nullopt;
        }

        auto action_token = trimmed.substr(action_separator + 1);
        std::ranges::transform(action_token, action_token.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        const auto action = magic_enum::enum_cast<approval_action>(action_token);
        if (!action.has_value()) {
            return std::nullopt;
        }

        return ParsedApprovalCallbackData{
            .request_id = trimmed.substr(request_start, action_separator - request_start),
            .action = *action,
        };
    }

    nlohmann::json build_approval_keyboard(std::string_view request_id, bool include_allow_always) {
        auto buttons = nlohmann::json::array();
        buttons.push_back(build_approval_button(0, request_id, approval_action::allow_once));
        if (include_allow_always) {
            buttons.push_back(build_approval_button(buttons.size(), request_id, approval_action::always_allow));
        }
        buttons.push_back(build_approval_button(buttons.size(), request_id, approval_action::deny));

        return nlohmann::json{
            {"content",
             {
                 {"rows", nlohmann::json::array({
                              {
                                  {"buttons", std::move(buttons)},
                              },
                          })},
             }},
        };
    }

} // namespace orangutan::channel::qq
