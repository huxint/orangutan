#pragma once

#include "channel/channel.hpp"
#include "channel/qq/qq-message-builder.hpp"
#include "channel/qq/qq-url.hpp"
#include "types/base.hpp"
#include "utils/string.hpp"

#include <array>
#include <concepts>
#include <nlohmann/json.hpp>

#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace orangutan::channel::qq {

    enum class qq_target_kind : std::uint8_t {
        c2c,
        group,
        guild,
    };

    struct qq_send_target {
        qq_target_kind kind;
        std::string id;
    };

    struct qq_media_segment {
        enum class kind : std::uint8_t {
            text,
            media,
        };

        kind segment_kind = kind::text;
        std::string content;
        int file_type = 1;
    };

    [[nodiscard]]
    inline std::string qq_jid_prefix(std::string_view bot_name, std::string_view kind) {
        if (bot_name.empty()) {
            return std::string("qqbot:") + std::string(kind) + ':';
        }
        return std::string("qqbot:") + std::string(bot_name) + ':' + std::string(kind) + ':';
    }

    [[nodiscard]]
    inline std::string make_qq_jid(std::string_view bot_name, std::string_view kind, std::string_view openid) {
        return qq_jid_prefix(bot_name, kind) + std::string(openid);
    }

    [[nodiscard]]
    inline std::string require_openid(std::string_view jid, std::string_view prefix) {
        if (!jid.starts_with(prefix)) {
            throw std::runtime_error("Unsupported QQ jid: " + std::string(jid));
        }
        return std::string(jid.substr(prefix.size()));
    }

    [[nodiscard]]
    inline qq_send_target resolve_send_target(std::string_view bot_name, std::string_view jid) {
        const auto c2c_prefix = qq_jid_prefix(bot_name, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name, "group");
        const auto guild_prefix = qq_jid_prefix(bot_name, "guild");

        if (jid.starts_with(c2c_prefix)) {
            return qq_send_target{.kind = qq_target_kind::c2c, .id = require_openid(jid, c2c_prefix)};
        }
        if (jid.starts_with(group_prefix)) {
            return qq_send_target{.kind = qq_target_kind::group, .id = require_openid(jid, group_prefix)};
        }
        if (jid.starts_with(guild_prefix)) {
            return qq_send_target{.kind = qq_target_kind::guild, .id = require_openid(jid, guild_prefix)};
        }

        throw std::runtime_error("Unsupported QQ jid: " + std::string(jid));
    }

    [[nodiscard]]
    inline std::optional<qq_send_target> resolve_typing_target(std::string_view bot_name, std::string_view jid) {
        try {
            const auto target = resolve_send_target(bot_name, jid);
            if (target.kind != qq_target_kind::c2c) {
                return std::nullopt;
            }
            return target;
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]]
    inline std::string message_path(const qq_send_target &target) {
        switch (target.kind) {
            case qq_target_kind::c2c:
                return "/v2/users/" + target.id + "/messages";
            case qq_target_kind::group:
                return "/v2/groups/" + target.id + "/messages";
            case qq_target_kind::guild:
                return "/channels/" + target.id + "/messages";
        }

        throw std::runtime_error("Unsupported QQ send target kind");
    }

    [[nodiscard]]
    inline std::string media_upload_path(const qq_send_target &target) {
        switch (target.kind) {
            case qq_target_kind::c2c:
                return "/v2/users/" + target.id + "/files";
            case qq_target_kind::group:
                return "/v2/groups/" + target.id + "/files";
            case qq_target_kind::guild:
                throw std::runtime_error("QQ guild currently does not support msg_type=7 media direct send");
        }

        throw std::runtime_error("Unsupported QQ send target kind");
    }

    [[nodiscard]]
    inline nlohmann::json build_typing_payload(std::uint16_t msg_seq, std::string_view message_id) {
        nlohmann::json payload = {
            {"msg_type", 6},
            {"input_notify", {{"input_type", 1}, {"input_second", 60}}},
            {"msg_seq", msg_seq},
        };
        if (!message_id.empty()) {
            payload["msg_id"] = message_id;
        }
        return payload;
    }

    [[nodiscard]]
    inline std::vector<std::string> chunk_text(const std::string &text, std::size_t limit) {
        if (text.empty() || text.size() <= limit) {
            return {text};
        }

        std::vector<std::string> chunks;
        std::string remaining = text;
        while (!remaining.empty()) {
            if (remaining.size() <= limit) {
                chunks.push_back(std::move(remaining));
                break;
            }

            std::size_t split_at = remaining.rfind('\n', limit);
            if (split_at == std::string::npos || split_at < limit / 2) {
                split_at = remaining.rfind(' ', limit);
            }
            if (split_at == std::string::npos || split_at < limit / 2) {
                split_at = limit;
            }

            chunks.push_back(remaining.substr(0, split_at));
            remaining.erase(0, split_at);
            remaining.erase(0, remaining.find_first_not_of(" \n\r\t"));
        }

        return chunks;
    }

    [[nodiscard]]
    inline bool text_contains_media_markup(std::string_view text) {
        return text.contains("<qq") || text.contains("![");
    }

    [[nodiscard]]
    inline std::vector<qq_media_segment> parse_media_segments(std::string_view text) {
        std::vector<qq_media_segment> segments;
        std::string remaining{text};

        const auto flush_text = [&segments](const std::string &segment_text) {
            auto trimmed = std::string(trim_copy(segment_text));
            if (!trimmed.empty()) {
                segments.push_back({.segment_kind = qq_media_segment::kind::text, .content = std::move(trimmed)});
            }
        };

        while (!remaining.empty()) {
            std::size_t best_pos = std::string::npos;
            std::string best_tag;
            int best_file_type = 1;

            struct tag_def {
                std::string_view open_tag;
                std::string_view close_tag;
                int file_type;
            };

            constexpr std::array<tag_def, 5> tags = {{
                {.open_tag = "<qqimg>", .close_tag = "</qqimg>", .file_type = 1},
                {.open_tag = "<qqimage>", .close_tag = "</qqimage>", .file_type = 1},
                {.open_tag = "<qqvoice>", .close_tag = "</qqvoice>", .file_type = 3},
                {.open_tag = "<qqvideo>", .close_tag = "</qqvideo>", .file_type = 2},
                {.open_tag = "<qqfile>", .close_tag = "</qqfile>", .file_type = 4},
            }};

            for (const auto &tag : tags) {
                const auto pos = remaining.find(tag.open_tag);
                if (pos != std::string::npos && pos < best_pos) {
                    const auto close_pos = remaining.find(tag.close_tag, pos + tag.open_tag.size());
                    if (close_pos != std::string::npos) {
                        best_pos = pos;
                        best_tag = remaining.substr(pos + tag.open_tag.size(), close_pos - pos - tag.open_tag.size());
                        best_file_type = tag.file_type;
                    }
                }
            }

            if (best_pos == std::string::npos) {
                const auto markdown_pos = remaining.find("![");
                if (markdown_pos != std::string::npos) {
                    const auto bracket_close = remaining.find("](", markdown_pos + 2);
                    if (bracket_close != std::string::npos) {
                        const auto paren_close = remaining.find(')', bracket_close + 2);
                        if (paren_close != std::string::npos) {
                            const auto url = remaining.substr(bracket_close + 2, paren_close - bracket_close - 2);
                            if (is_absolute_url(url)) {
                                flush_text(remaining.substr(0, markdown_pos));
                                segments.push_back({.segment_kind = qq_media_segment::kind::media, .content = std::string(trim_copy(url)), .file_type = 1});
                                remaining.erase(0, paren_close + 1);
                                continue;
                            }
                        }
                    }
                }

                flush_text(remaining);
                break;
            }

            flush_text(remaining.substr(0, best_pos));

            for (const auto &tag : tags) {
                if (tag.file_type == best_file_type) {
                    const auto open_pos = remaining.find(tag.open_tag, best_pos);
                    if (open_pos == best_pos) {
                        const auto close_pos = remaining.find(tag.close_tag, open_pos + tag.open_tag.size());
                        if (close_pos != std::string::npos) {
                            remaining.erase(0, close_pos + tag.close_tag.size());
                            break;
                        }
                    }
                }
            }

            auto media_url = std::string(trim_copy(best_tag));
            if (is_absolute_url(media_url)) {
                segments.push_back({.segment_kind = qq_media_segment::kind::media, .content = std::move(media_url), .file_type = best_file_type});
            }
        }

        return segments;
    }

    template <typename PostPayload, typename UploadMediaInfo, typename NextMsgSeq, typename ResolvePassiveReply>
    inline void route_outbound_payload(const qq_send_target &target, const OutboundMessage &message, PostPayload &&post_payload, UploadMediaInfo &&upload_media_info,
                                       NextMsgSeq &&next_msg_seq, ResolvePassiveReply &&resolve_passive_reply_message_id) {
        const auto send_chunked = [&](const std::string &content, auto &&builder_factory) {
            const auto chunks = chunk_text(content, 5000);
            const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, static_cast<int>(chunks.size()));
            const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
            for (const auto &chunk : chunks) {
                post_payload(builder_factory(chunk, passive_reply_message_id, effective_reference));
            }
        };

        std::visit(
            [&]<typename Payload>(const Payload &payload) {
                using payload_type = std::decay_t<Payload>;

                if constexpr (std::same_as<payload_type, TextPayload>) {
                    send_chunked(payload.text, [&](const std::string &chunk, const std::string &reply_to_message_id, const std::string &reference_message_id) {
                        return QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(reply_to_message_id).reference(reference_message_id).build();
                    });
                } else if constexpr (std::same_as<payload_type, MarkdownPayload>) {
                    send_chunked(payload.markdown, [&](const std::string &chunk, const std::string &reply_to_message_id, const std::string &reference_message_id) {
                        return QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(reply_to_message_id).reference(reference_message_id).build();
                    });
                } else if constexpr (std::same_as<payload_type, MediaPayload>) {
                    const auto file_info = upload_media_info(target, payload.file_type, payload.url);
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, 1);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(
                        QqMessageBuilder{}.media(file_info, payload.caption).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build());
                } else if constexpr (std::same_as<payload_type, KeyboardPayload>) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, 1);
                    post_payload(
                        QqMessageBuilder{}.markdown(payload.markdown).keyboard(payload.keyboard_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).build());
                } else if constexpr (std::same_as<payload_type, ArkPayload>) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, 1);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}.ark(payload.ark_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build());
                } else if constexpr (std::same_as<payload_type, EmbedPayload>) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, 1);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}.embed(payload.embed_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build());
                }
            },
            message.payload);
    }

    template <typename SendImmediately, typename EnqueueDebounced>
    inline void route_media_segments(std::string_view jid, const OutboundMessage &message, SendImmediately &&send_immediately, EnqueueDebounced &&enqueue_debounced) {
        const auto *text_payload = std::get_if<TextPayload>(&message.payload);
        if (text_payload == nullptr) {
            send_immediately(OutboundMessage{.payload = message.payload, .reply_to_message_id = message.reply_to_message_id, .reference_message_id = message.reference_message_id});
            return;
        }

        const auto segments = parse_media_segments(text_payload->text);
        const bool has_media = std::ranges::any_of(segments, [](const qq_media_segment &segment) {
            return segment.segment_kind == qq_media_segment::kind::media;
        });
        if (!has_media) {
            enqueue_debounced(text_payload->text, message.reply_to_message_id, message.reference_message_id);
            return;
        }

        for (const auto &segment : segments) {
            if (segment.segment_kind == qq_media_segment::kind::text) {
                send_immediately(OutboundMessage{
                    .payload = TextPayload{.text = segment.content},
                    .reply_to_message_id = message.reply_to_message_id,
                    .reference_message_id = message.reference_message_id,
                });
            } else {
                send_immediately(OutboundMessage{
                    .payload = MediaPayload{.file_type = segment.file_type, .url = segment.content},
                    .reply_to_message_id = message.reply_to_message_id,
                });
            }
        }
    }

} // namespace orangutan::channel::qq
