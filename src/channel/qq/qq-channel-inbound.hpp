#pragma once

#include "channel/channel.hpp"
#include "channel/qq/qq-channel-outbound.hpp"
#include "types/base.hpp"
#include "utils/string.hpp"

#include <ctre.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace orangutan::channel::qq {

    [[nodiscard]]
    std::int64_t parse_integer_like(const nlohmann::json &payload, std::string_view key, std::int64_t default_value);

    [[nodiscard]]
    inline std::vector<Attachment> parse_attachments(const nlohmann::json &data) {
        std::vector<Attachment> attachments;
        if (!data.contains("attachments") || !data.at("attachments").is_array()) {
            return attachments;
        }

        attachments.reserve(data.at("attachments").size());
        for (const auto &item : data.at("attachments")) {
            attachments.push_back(Attachment{
                .content_type = item.value("content_type", std::string{}),
                .url = item.value("url", std::string{}),
                .filename = item.value("filename", std::string{}),
                .width = static_cast<int>(parse_integer_like(item, "width", 0)),
                .height = static_cast<int>(parse_integer_like(item, "height", 0)),
                .size = static_cast<int>(parse_integer_like(item, "size", 0)),
                .download_pending = item.contains("url") && item.at("url").is_string() && !item.at("url").get_ref<const std::string &>().empty(),
            });
        }

        return attachments;
    }

    [[nodiscard]]
    inline std::vector<std::string> parse_mention_ids(const nlohmann::json &data) {
        std::vector<std::string> ids;
        if (!data.contains("mentions") || !data.at("mentions").is_array()) {
            return ids;
        }

        for (const auto &mention : data.at("mentions")) {
            const auto mention_id = mention.value("member_openid", mention.value("user_openid", mention.value("id", std::string{})));
            if (!mention_id.empty()) {
                ids.push_back(mention_id);
            }
        }

        return ids;
    }

    [[nodiscard]]
    inline bool is_bot_mentioned(std::string_view app_id, const nlohmann::json &data, const std::vector<std::string> &mention_ids) {
        if (mention_ids.empty()) {
            return false;
        }

        if (data.contains("mentions") && data.at("mentions").is_array()) {
            for (const auto &mention : data.at("mentions")) {
                if (mention.value("bot", false)) {
                    return true;
                }
                const auto mention_id = mention.value("id", std::string{});
                if (!mention_id.empty() && mention_id == app_id) {
                    return true;
                }
            }
        }

        return data.value("content", std::string{}).contains("<@");
    }

    [[nodiscard]]
    inline std::string strip_mentions(const std::string &content) {
        std::string stripped;
        stripped.reserve(content.size());
        const auto content_view = std::string_view{content};
        const char *cursor = content_view.data();
        const char *end = cursor + content_view.size();

        for (const auto match : ctre::search_all<R"(<@!?[^>]+>)">(content_view)) {
            const auto full = match.get<0>().to_view();
            stripped.append(cursor, full.data() - cursor);
            cursor = full.data() + full.size();
        }
        stripped.append(cursor, static_cast<std::size_t>(end - cursor));
        return std::string(trim_copy(stripped));
    }

    [[nodiscard]]
    inline std::string parse_message_scene_ext_value(const nlohmann::json &data, std::string_view key) {
        if (!data.contains("message_scene") || !data.at("message_scene").is_object()) {
            return {};
        }

        const auto &scene = data.at("message_scene");
        if (!scene.contains("ext") || !scene.at("ext").is_array()) {
            return {};
        }

        const std::string prefix = std::string(key) + "=";
        for (const auto &item : scene.at("ext")) {
            if (!item.is_string()) {
                continue;
            }
            const auto &value = item.get_ref<const std::string &>();
            if (value.starts_with(prefix)) {
                return value.substr(prefix.size());
            }
        }

        return {};
    }

    [[nodiscard]]
    inline InboundMessage make_c2c_inbound_message(std::string_view bot_name, std::string_view openid, std::string_view sender_id, const nlohmann::json &data,
                                                   const std::string &referenced_content) {
        const auto message_id = data.value("id", std::string{});
        const auto ref_msg_idx = parse_message_scene_ext_value(data, "ref_msg_idx");
        return InboundMessage{
            .jid = make_qq_jid(bot_name, "c2c", openid),
            .sender = std::string(sender_id),
            .sender_name = std::string(openid),
            .content = data.value("content", std::string{}),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = ref_msg_idx,
            .referenced_content = referenced_content,
            .attachments = parse_attachments(data),
            .is_group = false,
        };
    }

    [[nodiscard]]
    inline InboundMessage make_group_inbound_message(std::string_view bot_name, std::string_view group_openid, std::string_view sender_id, std::string_view sender_name,
                                                     const nlohmann::json &data, const std::string &aggregated_content, const std::string &referenced_content,
                                                     const std::vector<std::string> &mention_ids, bool mentioned) {
        const auto message_id = data.value("id", std::string{});
        const auto ref_msg_idx = parse_message_scene_ext_value(data, "ref_msg_idx");
        return InboundMessage{
            .jid = make_qq_jid(bot_name, "group", group_openid),
            .sender = std::string(sender_id),
            .sender_name = std::string(sender_name),
            .content = aggregated_content,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = ref_msg_idx,
            .referenced_content = referenced_content,
            .attachments = parse_attachments(data),
            .mentioned = mentioned,
            .mention_ids = mention_ids,
            .is_group = true,
        };
    }

    [[nodiscard]]
    inline InboundMessage make_guild_inbound_message(std::string_view bot_name, std::string_view channel_id, std::string_view sender_id, std::string_view sender_name,
                                                     const nlohmann::json &data, const std::string &referenced_content, const std::vector<std::string> &mention_ids,
                                                     bool mentioned) {
        const auto message_id = data.value("id", std::string{});
        const auto ref_msg_idx = parse_message_scene_ext_value(data, "ref_msg_idx");
        return InboundMessage{
            .jid = make_qq_jid(bot_name, "guild", channel_id),
            .sender = std::string(sender_id),
            .sender_name = std::string(sender_name),
            .content = strip_mentions(data.value("content", std::string{})),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = ref_msg_idx,
            .referenced_content = referenced_content,
            .attachments = parse_attachments(data),
            .mentioned = mentioned,
            .mention_ids = mention_ids,
            .is_group = true,
        };
    }

    [[nodiscard]]
    inline InboundMessage make_reaction_inbound_message(std::string_view bot_name, std::string_view event_type, const nlohmann::json &data) {
        const auto target = data.value("target", nlohmann::json::object());
        const auto emoji = data.value("emoji", nlohmann::json::object());
        const auto channel_id = data.value("channel_id", std::string{});
        const auto user_id = data.value("user_id", std::string{});
        return InboundMessage{
            .event_kind = event_type == "MESSAGE_REACTION_ADD" ? inbound_event_kind::reaction_added : inbound_event_kind::reaction_removed,
            .jid = make_qq_jid(bot_name, "guild", channel_id),
            .sender = user_id,
            .sender_name = user_id,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = target.value("id", data.value("message_id", std::string{})),
            .reaction =
                ReactionInfo{
                    .user_id = user_id,
                    .target_id = target.value("id", std::string{}),
                    .target_type = static_cast<int>(parse_integer_like(target, "type", 0)),
                    .emoji_id = emoji.value("id", std::string{}),
                    .emoji_type = static_cast<int>(parse_integer_like(emoji, "type", 0)),
                },
            .is_group = true,
        };
    }

} // namespace orangutan::channel::qq
