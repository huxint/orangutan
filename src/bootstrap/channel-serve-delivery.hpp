#pragma once

#include "channel/channel.hpp"

#include <string>

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

    inline void deliver_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
        if (reply.empty()) {
            return;
        }

        const auto target = resolve_reply_target(message);
        if (target == "cli") {
            spdlog::info("Heartbeat reply [{}]: {}", message.jid, reply);
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
            spdlog::error("Failed to deliver reply for jid '{}' to target '{}': {}", message.jid, target, e.what());
        }
    }

    inline void deliver_command_reply(const InboundMessage &message, const std::string &reply, ChannelManager &channel_manager) {
        deliver_reply(message, reply, channel_manager);
    }

} // namespace orangutan::bootstrap
