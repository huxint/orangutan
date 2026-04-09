#include "channel/qq/qq-approval-keyboard.hpp"

#include <algorithm>
#include <cctype>
#include <magic_enum/magic_enum.hpp>

namespace orangutan::channel::qq {

    namespace {

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
        std::string approval_action_label(approval_action action) {
            std::string label{magic_enum::enum_name(action)};
            std::ranges::replace(label, '_', ' ');
            return label;
        }

        [[nodiscard]]
        nlohmann::json build_approval_button(std::size_t index, std::string_view request_id, approval_action action) {
            const auto label = approval_action_label(action);
            return nlohmann::json{
                {"id", std::to_string(index)},
                {"render_data",
                 {
                     {"label", label},
                     {"visited_label", label},
                 }},
                {"action",
                 {
                     {"type", 2},
                     {"permission", {{"type", 2}}},
                     {"enter", true},
                     {"data", build_approval_callback_data(request_id, action)},
                 }},
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
                 {"rows",
                  nlohmann::json::array({
                      {
                          {"buttons", std::move(buttons)},
                      },
                  })},
             }},
        };
    }

} // namespace orangutan::channel::qq
