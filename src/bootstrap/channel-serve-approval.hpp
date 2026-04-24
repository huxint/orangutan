#pragma once

#include "bootstrap/channel-serve-delivery.hpp"
#include "channel/qq/qq-approval-keyboard.hpp"
#include "permissions/permission-display.hpp"
#include "permissions/permission-evaluator.hpp"
#include "tools/registry/tool-context.hpp"
#include "tools/registry/tool-registry.hpp"
#include "types/base.hpp"
#include "types/content.hpp"
#include "utils/format.hpp"
#include "utils/string.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan::bootstrap::detail {

    enum class channel_approval_decision : std::uint8_t {
        approve_once,
        approve_always,
        deny,
    };

    struct ParsedChannelApprovalReply {
        std::string request_id;
        channel_approval_decision decision;
    };

    inline constexpr std::string_view CHANNEL_APPROVAL_REQUEST_PREFIX = "tool-approval-";
    inline constexpr std::string_view QQ_APPROVAL_TITLE = "## Approval required";
    inline constexpr std::string_view QQ_TOOL_TITLE = "## Tool";
    inline constexpr std::string_view QQ_FIELD_DIR = "dir";
    inline constexpr std::string_view QQ_FIELD_TIMEOUT = "timeout";

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
        if (bot_name.empty()) {
            return "default:unnamed";
        }

        return fmt::format("named:{}", bot_name);
    }

    [[nodiscard]]
    inline bool is_qq_custom_keyboard_blocked_error(std::string_view message) {
        const auto lowered = utils::ascii_to_lower_copy(message);
        return lowered.contains("biz_code=304057") || lowered.contains("not allowd custom keyborad") || lowered.contains("not allowed custom keyboard");
    }

    [[nodiscard]]
    inline std::string trim_ascii_copy(std::string_view value) {
        return std::string(utils::trim_copy(value));
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

    inline void append_qq_compact_field(std::string &markdown, std::string_view label, std::string_view value) {
        if (value.empty()) {
            return;
        }
        utils::format_to(markdown, "- {}: `{}`\n", label, value);
    }

    inline void append_qq_tool_preview(std::string &markdown, const ToolUse &call) {
        if (const auto command = first_scalar_input_value(call.input, {"command"}); command.has_value()) {
            utils::format_to(markdown, "\n```bash\n{}\n```", *command);
        } else if (const auto preview = first_scalar_input_value(call.input, {"path", "file_path", "query", "prompt", "url"}); preview.has_value()) {
            utils::format_to(markdown, "\n`{}`\n", *preview);
        } else if (call.input.is_object() && !call.input.empty()) {
            utils::format_to(markdown, "\n```json\n{}\n```", call.input.dump(2));
        }
    }

    inline void append_qq_tool_metadata(std::string &markdown, const ToolUse &call) {
        if (const auto directory = first_scalar_input_value(call.input, {"working_dir", "cwd"}); directory.has_value()) {
            append_qq_compact_field(markdown, QQ_FIELD_DIR, *directory);
        }
        if (const auto timeout_seconds = first_scalar_input_value(call.input, {"timeout_seconds", "timeout_secs"}); timeout_seconds.has_value()) {
            const auto value = fmt::format("{}s", *timeout_seconds);
            append_qq_compact_field(markdown, QQ_FIELD_TIMEOUT, value);
        } else if (const auto timeout = first_scalar_input_value(call.input, {"timeout"}); timeout.has_value()) {
            append_qq_compact_field(markdown, QQ_FIELD_TIMEOUT, *timeout);
        } else if (const auto timeout_ms = first_scalar_input_value(call.input, {"timeout_ms"}); timeout_ms.has_value()) {
            const auto value = fmt::format("{}ms", *timeout_ms);
            append_qq_compact_field(markdown, QQ_FIELD_TIMEOUT, value);
        }
    }

    [[nodiscard]]
    inline std::string format_channel_approval_request_id(std::uint64_t prompt_id) {
        return fmt::format("{}{}", CHANNEL_APPROVAL_REQUEST_PREFIX, prompt_id);
    }

    [[nodiscard]]
    inline std::string format_qq_channel_approval_card_markdown(const ToolUse &call, const PermissionDecision &decision) {
        std::string markdown{QQ_APPROVAL_TITLE};
        const auto prompt = permissions::approval_prompt_message(decision);
        if (!prompt.empty()) {
            utils::format_to(markdown, "\n{}", prompt);
        }

        markdown += "\n";
        append_qq_tool_preview(markdown, call);

        std::string metadata;
        append_qq_tool_metadata(metadata, call);
        if (!metadata.empty()) {
            utils::format_to(markdown, "\n{}", metadata);
        }

        for (const auto &line : permissions::permission_decision_detail_lines(decision)) {
            const auto separator = line.find(':');
            if (separator == std::string::npos) {
                continue;
            }
            const auto label = trim_ascii_copy(std::string_view(line).substr(0, separator));
            const auto value = trim_ascii_copy(std::string_view(line).substr(separator + 1));
            if (label == "Rule" || label == "Path" || label == "Detail") {
                append_qq_compact_field(markdown, utils::ascii_to_lower_copy(label), value);
            }
        }
        return markdown;
    }

    [[nodiscard]]
    inline std::string format_qq_tool_progress_markdown(const ToolUse &call) {
        auto markdown = fmt::format("{}\n`{}`", QQ_TOOL_TITLE, call.name);
        append_qq_tool_preview(markdown, call);

        std::string metadata;
        append_qq_tool_metadata(metadata, call);
        if (!metadata.empty()) {
            utils::format_to(markdown, "\n{}", metadata);
        }
        return markdown;
    }

    [[nodiscard]]
    inline std::string format_qq_approval_delivery_failure(const ToolUse &call, const PermissionDecision &decision, bool keyboard_unavailable) {
        std::string message = permissions::approval_prompt_message(decision);
        utils::format_to(message, "\n{}",
                         keyboard_unavailable ? "QQ approval buttons are unavailable for this bot account, so the tool call was rejected."
                                              : "Failed to deliver the QQ approval card, so the tool call was rejected.");
        utils::format_to(message, "\nTool: {}", call.name);
        if (const auto command = first_scalar_input_value(call.input, {"command"}); command.has_value()) {
            utils::format_to(message, "\nCommand: {}", *command);
        }
        return message;
    }

    [[nodiscard]]
    inline std::optional<ParsedChannelApprovalReply> parse_channel_approval_reply(std::string_view content) {
        if (const auto callback = channel::qq::parse_approval_callback_data(content); callback.has_value()) {
            switch (callback->action) {
                case channel::qq::approval_action::allow_once:
                    return ParsedChannelApprovalReply{
                        .request_id = callback->request_id,
                        .decision = channel_approval_decision::approve_once,
                    };
                case channel::qq::approval_action::always_allow:
                    return ParsedChannelApprovalReply{
                        .request_id = callback->request_id,
                        .decision = channel_approval_decision::approve_always,
                    };
                case channel::qq::approval_action::deny:
                    return ParsedChannelApprovalReply{
                        .request_id = callback->request_id,
                        .decision = channel_approval_decision::deny,
                    };
            }
        }
        return std::nullopt;
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

        return target == message.jid && is_qq_channel_target(target);
    }

    [[nodiscard]]
    inline bool should_send_qq_tool_progress_at_start(const ToolUse &call, const ToolRegistry &tools, const ToolRuntimeContext &tool_context) {
        if (tool_context.abort_checker && tool_context.abort_checker()) {
            return false;
        }

        const auto *permission_context = tool_context.permission_context;
        if (permission_context == nullptr) {
            return true;
        }

        const auto *tool = tools.find_tool(call.name);
        permissions::ToolPermissionChecker checker;
        permissions::IsReadOnlyChecker is_read_only;
        if (tool != nullptr) {
            checker = tool->check_permissions;
            is_read_only = [tool] {
                return tool->read_only;
            };
        }

        auto decision = permissions::evaluate_permission(call, *permission_context, checker, is_read_only);
        decision = permissions::apply_post_processing(decision, permission_context->mode);
        return decision.behavior == permission_behavior::allow;
    }

} // namespace orangutan::bootstrap::detail
