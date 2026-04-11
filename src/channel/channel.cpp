#include "channel/channel.hpp"

#include <algorithm>
#include <iterator>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan::channel {

    namespace {

        Channel &require_channel_for_jid(const std::vector<std::unique_ptr<Channel>> &channels, std::string_view jid) {
            for (const auto &channel : channels) {
                if (channel->owns_jid(std::string{jid})) {
                    return *channel;
                }
            }

            throw std::runtime_error("No channel owns jid: " + std::string{jid});
        }

    } // namespace

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

    void ChannelManager::send(const std::string &jid, const OutboundMessage &message) {
        require_channel_for_jid(channels_, jid).send(jid, message);
    }

    void ChannelManager::send(std::string_view jid, std::string_view text, std::string_view reply_to_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload = TextPayload{.text = std::string{text}},
                                   .reply_to_message_id = std::string{reply_to_message_id},
                               });
    }

    void ChannelManager::send_markdown(std::string_view jid, std::string_view markdown, std::string_view reply_to_message_id, std::string_view reference_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload = MarkdownPayload{.markdown = std::string{markdown}},
                                   .reply_to_message_id = std::string{reply_to_message_id},
                                   .reference_message_id = std::string{reference_message_id},
                               });
    }

    void ChannelManager::send_media(std::string_view jid, int file_type, std::string_view url, std::string_view reply_to_message_id, std::string_view caption,
                                    std::string_view reference_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload =
                                       MediaPayload{
                                           .file_type = file_type,
                                           .url = std::string{url},
                                           .caption = std::string{caption},
                                       },
                                   .reply_to_message_id = std::string{reply_to_message_id},
                                   .reference_message_id = std::string{reference_message_id},
                               });
    }

    void ChannelManager::send_keyboard(std::string_view jid, std::string_view markdown, const nlohmann::json &keyboard_payload, std::string_view reply_to_message_id,
                                       std::string_view reference_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload =
                                       KeyboardPayload{
                                           .markdown = std::string{markdown},
                                           .keyboard_payload = keyboard_payload,
                                       },
                                   .reply_to_message_id = std::string{reply_to_message_id},
                                   .reference_message_id = std::string{reference_message_id},
                               });
    }

    void ChannelManager::send_ark(std::string_view jid, const nlohmann::json &ark_payload, std::string_view reply_to_message_id, std::string_view reference_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload = ArkPayload{.ark_payload = ark_payload},
                                   .reply_to_message_id = std::string{reply_to_message_id},
                                   .reference_message_id = std::string{reference_message_id},
                               });
    }

    void ChannelManager::send_embed(std::string_view jid, const nlohmann::json &embed_payload, std::string_view reply_to_message_id, std::string_view reference_message_id) {
        send(std::string{jid}, OutboundMessage{
                                   .payload = EmbedPayload{.embed_payload = embed_payload},
                                   .reply_to_message_id = std::string{reply_to_message_id},
                                   .reference_message_id = std::string{reference_message_id},
                               });
    }

    Attachment ChannelManager::download_attachment(std::string_view jid, const Attachment &attachment, const std::filesystem::path &destination_path) {
        return require_channel_for_jid(channels_, jid).download_attachment(jid, attachment, destination_path);
    }

    void ChannelManager::add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
        require_channel_for_jid(channels_, jid).add_reaction(jid, message_id, type, id);
    }

    void ChannelManager::remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
        require_channel_for_jid(channels_, jid).remove_reaction(jid, message_id, type, id);
    }

    void ChannelManager::start_typing(const std::string &jid, const std::string &message_id) {
        try {
            require_channel_for_jid(channels_, jid).start_typing(jid, message_id);
        } catch (const std::exception &e) {
            spdlog::debug("start_typing failed for jid '{}': {}", jid, e.what());
        }
    }

    void ChannelManager::stop_typing(const std::string &jid) {
        try {
            require_channel_for_jid(channels_, jid).stop_typing(jid);
        } catch (const std::exception &e) {
            spdlog::debug("stop_typing failed for jid '{}': {}", jid, e.what());
        }
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

    std::vector<std::string> ChannelManager::known_user_jids() const {
        std::vector<std::string> jids;
        for (const auto &channel : channels_) {
            auto channel_jids = channel->known_user_jids();
            jids.insert(jids.end(), std::make_move_iterator(channel_jids.begin()), std::make_move_iterator(channel_jids.end()));
        }
        std::ranges::sort(jids);
        jids.erase(std::ranges::unique(jids).begin(), jids.end());
        return jids;
    }

} // namespace orangutan::channel
