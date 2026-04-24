#pragma once

#include "channel/channel.hpp"
#include "utils/format.hpp"

#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace orangutan::bootstrap {

    [[nodiscard]]
    inline std::string resolve_reply_target(const InboundMessage &message) {
        if (message.reply_target == "cli") {
            return "cli";
        }
        if (message.reply_target.empty()) {
            if (message.jid.starts_with("heartbeat:")) {
                return "cli";
            }
            return message.jid;
        }
        return message.reply_target;
    }

    [[nodiscard]]
    inline OutboundMessage make_qq_stream_progress_message(std::string text) {
        return OutboundMessage{
            .payload = TextPayload{.text = std::move(text)},
        };
    }

    [[nodiscard]]
    inline OutboundMessage make_qq_stream_final_message(const InboundMessage &message, std::string text) {
        return OutboundMessage{
            .payload = TextPayload{.text = std::move(text)},
            .reply_to_message_id = message.message_id,
            .reference_message_id = message.message_id,
        };
    }

    [[nodiscard]]
    inline std::string format_qq_reply_markdown(std::string_view thinking, std::string_view reply) {
        if (thinking.empty()) {
            return std::string(reply);
        }

        std::string markdown{"> thinking\n"};
        std::size_t line_start = 0;
        while (line_start < thinking.size()) {
            const auto line_end = thinking.find('\n', line_start);
            const auto line = thinking.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
            utils::format_to(markdown, "> {}\n", line);
            if (line_end == std::string_view::npos) {
                break;
            }
            line_start = line_end + 1;
        }

        if (!reply.empty()) {
            utils::format_to(markdown, "\n{}", reply);
        }
        return markdown;
    }

    inline void deliver_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
        if (reply.empty()) {
            return;
        }

        const auto target = resolve_reply_target(message);
        if (target == "cli") {
            spdlog::info("heartbeat reply [{}]: {}", message.jid, reply);
            return;
        }

        try {
            if (target.starts_with("qqbot:")) {
                channel_manager.send(target, OutboundMessage{
                                                 .payload = TextPayload{.text = reply},
                                                 .reply_to_message_id = message.message_id,
                                                 .reference_message_id = message.message_id,
                                             });
            } else {
                channel_manager.send(target, reply, message.message_id);
            }
        } catch (const std::exception &e) {
            spdlog::error("failed to deliver reply for jid '{}' to target '{}': {}", message.jid, target, e.what());
        }
    }

    inline void deliver_command_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
        deliver_reply(message, reply, channel_manager);
    }

} // namespace orangutan::bootstrap
