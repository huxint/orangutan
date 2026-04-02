#include "channel/channel.hpp"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan::channel {

    ChannelManager::ChannelManager(Allowlist allowlist)
    : allowlist_(std::move(allowlist)) {}

    void ChannelManager::add_channel(std::unique_ptr<Channel> ch) {
        channels_.push_back(std::move(ch));
    }

    void ChannelManager::connect_all(const MessageCallback &on_message) {
        for (const auto &channel : channels_) {
            channel->connect([this, on_message](const InboundMessage &message) {
                if (!allowlist_.is_allowed(message.jid)) {
                    spdlog::warn("Dropping inbound message from denied jid '{}'", message.jid);
                    return;
                }

                if (on_message != nullptr) {
                    on_message(message);
                }
            });
        }
    }

    void ChannelManager::send(const std::string &jid, const std::string &text, const std::string &reply_to_message_id) {
        for (const auto &channel : channels_) {
            if (channel->owns_jid(jid)) {
                channel->send_message(jid, text, reply_to_message_id);
                return;
            }
        }

        throw std::runtime_error("No channel owns jid: " + jid);
    }

    void ChannelManager::disconnect_all() {
        for (const auto &channel : channels_) {
            if (!channel->is_connected()) {
                continue;
            }

            try {
                channel->disconnect();
            } catch (const std::exception &e) {
                spdlog::warn("Failed to disconnect channel '{}': {}", channel->name(), e.what());
            }
        }
    }

    bool ChannelManager::has_channels() const {
        return !channels_.empty();
    }

} // namespace orangutan::channel
