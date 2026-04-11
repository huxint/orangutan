#pragma once

#include "bootstrap/channel-serve-delivery.hpp"
#include "channel/qq/qq-approval-keyboard.hpp"
#include "permissions/permission-display.hpp"
#include "types/base.hpp"
#include "types/types.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan::bootstrap::detail {

    enum class channel_approval_decision : std::uint8_t {
        approve_once,
        approve_always,
        deny,
        invalid,
    };

    struct ParsedChannelApprovalReply {
        std::string request_id;
        channel_approval_decision decision = channel_approval_decision::invalid;
    };

    [[nodiscard]]
    inline std::string extract_qq_bot_name(const std::string &jid) {
        constexpr std::string_view PREFIX = "qqbot:";
        if (!jid.starts_with(PREFIX)) {
            return {};
        }

        const auto remainder = jid.substr(PREFIX.size());
        const auto first_colon = remainder.find(':');
        if (first_colon == std::string::npos) {
            return {};
        }

        auto first_segment = remainder.substr(0, first_colon);
        if (first_segment == "c2c" || first_segment == "group") {
            return {};
        }

        return {first_segment};
    }

    [[nodiscard]]
    inline bool is_qq_channel_target(std::string_view target) {
        return target.starts_with("qqbot:");
    }

    [[nodiscard]]
    inline std::string qq_keyboard_capability_key(std::string_view target) {
        const auto bot_name = extract_qq_bot_name(std::string(target));
        return bot_name.empty() ? std::string{"legacy:default"} : "named:" + bot_name;
    }

    [[nodiscard]]
    inline bool is_qq_custom_keyboard_blocked_error(std::string_view message) {
        const auto lowered = utils::overwrite_string(message.size(), [message](char *buffer, std::size_t size) {
            for (std::size_t index = 0; index < size; ++index) {
                buffer[index] = utils::ascii_to_lower_char(static_cast<unsigned char>(message[index]));
            }
            return size;
        });
        return lowered.contains("biz_code=304057") || lowered.contains("not allowd custom keyborad") || lowered.contains("not allowed custom keyboard");
    }

    [[nodiscard]]
    inline std::string trim_ascii_copy(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    [[nodiscard]]
    inline std::optional<std::string> json_scalar_as_string(const nlohmann::json &value) {
        if (value.is_string()) {
            auto text = trim_ascii_copy(value.get_ref<const std::string &>());
            return text.empty() ? std::nullopt : std::optional<std::string>{std::move(text)};
        }
        if (value.is_number_integer()) {
            return std::to_string(value.get<long long>());
        }
        if (value.is_number_unsigned()) {
            return std::to_string(value.get<unsigned long long>());
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? std::string{"true"} : std::string{"false"};
        }
        return std::nullopt;
    }

    [[nodiscard]]
    inline std::optional<std::string> first_scalar_input_value(const nlohmann::json &input, std::initializer_list<std::string_view> keys) {
        if (!input.is_object()) {
            return std::nullopt;
        }

        for (const auto key : keys) {
            const auto it = input.find(static_cast<std::string>(key));
            if (it == input.end()) {
                continue;
            }
            if (const auto value = json_scalar_as_string(*it).and_then([](std::string text) -> std::optional<std::string> {
                    return text.empty() ? std::nullopt : std::optional<std::string>{std::move(text)};
                });
                value.has_value()) {
                return value;
            }
        }
        return std::nullopt;
    }

    inline void append_qq_card_field(std::string &markdown, std::string_view icon, std::string_view label, std::string_view value, bool inline_code = true) {
        if (value.empty()) {
            return;
        }
        markdown += "- ";
        markdown += icon;
        markdown += " ";
        markdown += label;
        markdown += ": ";
        if (inline_code) {
            markdown.push_back('`');
            markdown += value;
            markdown.push_back('`');
        } else {
            markdown += value;
        }
        markdown.push_back('\n');
    }

    inline void append_qq_decision_detail(std::string &markdown, std::string_view line) {
        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            append_qq_card_field(markdown, "ℹ️", "Detail", trim_ascii_copy(line), false);
            return;
        }

        const auto label = trim_ascii_copy(line.substr(0, separator));
        const auto value = trim_ascii_copy(line.substr(separator + 1));
        if (label.empty() || value.empty() || label == "Behavior") {
            return;
        }

        std::string_view icon = "ℹ️";
        if (label == "Reason") {
            icon = "📝";
        } else if (label == "Rule") {
            icon = "📏";
        } else if (label == "Mode") {
            icon = "⚙️";
        } else if (label == "Path") {
            icon = "📂";
        } else if (label == "Detail") {
            icon = "📌";
        }
        append_qq_card_field(markdown, icon, label, value);
    }

    [[nodiscard]]
    inline std::string format_channel_approval_request_id(base::u64 prompt_id) {
        return "tool-approval-" + std::to_string(prompt_id);
    }

    [[nodiscard]]
    inline std::string format_channel_approval_text_reply(std::string_view request_id, bool include_allow_always) {
        if (include_allow_always) {
            return "reply with `" + std::string(request_id) + " yes`, `" + std::string(request_id) + " always`, or `" + std::string(request_id) + " no`.";
        }
        return "reply with `" + std::string(request_id) + " yes` or `" + std::string(request_id) + " no`.";
    }

    [[nodiscard]]
    inline std::string format_qq_channel_approval_card_markdown(const ToolUse &call, const PermissionDecision &decision) {
        std::string markdown = call.name == "shell" ? "## 🔐 Command Execution Approval" : "## 🔐 Tool Approval";
        const auto prompt = permissions::approval_prompt_message(decision);
        if (!prompt.empty()) {
            markdown += "\n> ";
            markdown += prompt;
        }

        if (const auto command = first_scalar_input_value(call.input, {"command"}); command.has_value()) {
            markdown += "\n\n```bash\n";
            markdown += *command;
            markdown += "\n```";
        } else if (const auto preview = first_scalar_input_value(call.input, {"path", "file_path", "query", "prompt", "url"}); preview.has_value()) {
            markdown += "\n\n> `";
            markdown += *preview;
            markdown += '`';
        } else if (call.input.is_object() && !call.input.empty()) {
            markdown += "\n\n```json\n";
            markdown += call.input.dump(2);
            markdown += "\n```";
        }

        markdown += "\n\n";
        append_qq_card_field(markdown, "🧰", "Tool", call.name);
        if (const auto directory = first_scalar_input_value(call.input, {"working_dir", "cwd"}); directory.has_value()) {
            append_qq_card_field(markdown, "📁", "Directory", *directory);
        }
        if (const auto agent = first_scalar_input_value(call.input, {"agent", "agent_key"}); agent.has_value()) {
            append_qq_card_field(markdown, "🤖", "Agent", *agent);
        }
        if (const auto timeout_seconds = first_scalar_input_value(call.input, {"timeout_seconds", "timeout_secs"}); timeout_seconds.has_value()) {
            append_qq_card_field(markdown, "⏱", "Timeout", *timeout_seconds + "s");
        } else if (const auto timeout = first_scalar_input_value(call.input, {"timeout"}); timeout.has_value()) {
            append_qq_card_field(markdown, "⏱", "Timeout", *timeout);
        } else if (const auto timeout_ms = first_scalar_input_value(call.input, {"timeout_ms"}); timeout_ms.has_value()) {
            append_qq_card_field(markdown, "⏱", "Timeout", *timeout_ms + "ms");
        }

        for (const auto &line : permissions::permission_decision_detail_lines(decision)) {
            append_qq_decision_detail(markdown, line);
        }
        return markdown;
    }

    [[nodiscard]]
    inline std::string format_text_channel_approval_prompt(const ToolUse &call, const PermissionDecision &decision, const std::string &request_id, bool include_allow_always) {
        std::string prompt = permissions::approval_prompt_message(decision);
        prompt += "\nTool: " + call.name;
        if (call.input.is_object()) {
            if (const auto it = call.input.find("command"); it != call.input.end() && it->is_string()) {
                prompt += "\nCommand: " + it->get<std::string>();
            }
        }
        for (const auto &line : permissions::permission_decision_detail_lines(decision)) {
            prompt += "\n" + line;
        }
        prompt += "\nRequest: " + request_id;
        prompt += "\nPlease ";
        prompt += format_channel_approval_text_reply(request_id, include_allow_always);
        return prompt;
    }

    [[nodiscard]]
    inline std::string format_qq_approval_delivery_failure(const ToolUse &call, const PermissionDecision &decision, bool keyboard_unavailable) {
        std::string message = permissions::approval_prompt_message(decision);
        message += keyboard_unavailable ? "\nQQ approval buttons are unavailable for this bot account, so the tool call was rejected."
                                        : "\nFailed to deliver the QQ approval card, so the tool call was rejected.";
        message += "\nTool: " + call.name;
        if (const auto command = first_scalar_input_value(call.input, {"command"}); command.has_value()) {
            message += "\nCommand: " + *command;
        }
        return message;
    }

    [[nodiscard]]
    inline std::string normalize_channel_approval_token(std::string_view content) {
        std::string normalized;
        normalized.reserve(content.size());
        for (const auto ch : content) {
            const auto lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (std::isalnum(static_cast<unsigned char>(lowered)) != 0 || lowered == '-') {
                normalized.push_back(lowered);
            }
        }
        return normalized;
    }

    [[nodiscard]]
    inline channel_approval_decision parse_channel_approval_decision(std::string_view content) {
        const auto normalized = normalize_channel_approval_token(content);
        if (normalized.empty()) {
            return channel_approval_decision::invalid;
        }

        if (normalized == "always" || normalized == "alwaysallow" || normalized == "always-allow" || normalized == "allowalways" || normalized == "allow-always") {
            return channel_approval_decision::approve_always;
        }
        if (normalized == "y" || normalized == "yes" || normalized == "approve" || normalized == "approved" || normalized == "allow") {
            return channel_approval_decision::approve_once;
        }
        if (normalized == "n" || normalized == "no" || normalized == "deny" || normalized == "denied" || normalized == "reject") {
            return channel_approval_decision::deny;
        }
        return channel_approval_decision::invalid;
    }

    [[nodiscard]]
    inline ParsedChannelApprovalReply parse_channel_approval_reply(const std::string &content, bool allow_text_reply) {
        ParsedChannelApprovalReply parsed;
        if (const auto callback = channel::qq::parse_approval_callback_data(content); callback.has_value()) {
            parsed.request_id = callback->request_id;
            switch (callback->action) {
                case channel::qq::approval_action::allow_once:
                    parsed.decision = channel_approval_decision::approve_once;
                    break;
                case channel::qq::approval_action::always_allow:
                    parsed.decision = channel_approval_decision::approve_always;
                    break;
                case channel::qq::approval_action::deny:
                    parsed.decision = channel_approval_decision::deny;
                    break;
            }
            return parsed;
        }

        if (!allow_text_reply) {
            return parsed;
        }

        std::istringstream stream(content);
        for (std::string token; static_cast<bool>(stream >> token);) {
            const auto normalized = normalize_channel_approval_token(token);
            if (normalized.starts_with("tool-approval-") || normalized.starts_with("shell-approval-")) {
                parsed.request_id = normalized;
                continue;
            }

            const auto decision = parse_channel_approval_decision(normalized);
            if (decision == channel_approval_decision::invalid) {
                continue;
            }
            if (parsed.decision == channel_approval_decision::approve_always && decision == channel_approval_decision::approve_once &&
                (normalized == "allow" || normalized == "approved")) {
                continue;
            }
            parsed.decision = decision;
        }
        return parsed;
    }

    [[nodiscard]]
    inline std::string format_pending_channel_approval_prompt(const std::vector<std::string> &request_ids, bool allow_text_reply) {
        if (request_ids.empty()) {
            return "Tool approval is pending.";
        }

        if (!allow_text_reply) {
            return request_ids.size() == 1 ? "Tool approval is pending. Use the buttons on the approval card."
                                           : "Multiple tool approvals are pending. Use the buttons on the approval cards.";
        }

        if (request_ids.size() == 1) {
            return "Tool approval is pending. Reply with `" + request_ids.front() + " yes` or `" + request_ids.front() + " no`.";
        }

        std::string prompt = "Multiple tool approvals are pending. Reply with `<request-id> yes` or `<request-id> no`. Pending:";
        for (const auto &request_id : request_ids) {
            utils::format_to(prompt, " {}", request_id);
        }
        return prompt;
    }

    [[nodiscard]]
    inline std::vector<std::string> pending_request_ids_for_jid(const std::unordered_map<std::string, std::vector<std::string>> &pending_request_ids_by_jid,
                                                                const std::string &jid) {
        const auto it = pending_request_ids_by_jid.find(jid);
        if (it == pending_request_ids_by_jid.end()) {
            return {};
        }
        return it->second;
    }

    [[nodiscard]]
    inline bool can_prompt_for_channel_approval(const InboundMessage &message) {
        if (message.jid.starts_with("heartbeat:")) {
            return false;
        }

        const auto target = resolve_reply_target(message);
        if (target == "cli") {
            return false;
        }

        return target == message.jid;
    }

} // namespace orangutan::bootstrap::detail
