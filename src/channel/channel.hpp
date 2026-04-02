#pragma once

#include "channel/allowlist.hpp"

#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace orangutan::channel {

    enum class InboundEventKind {
        message,
        reaction_added,
        reaction_removed,
    };

    struct Attachment {
        std::string content_type;
        std::string url;
        std::string filename;
        int width = 0;
        int height = 0;
        int size = 0;
        bool download_pending = false;
        std::string local_path;
        std::string download_error;
    };

    struct ReactionInfo {
        std::string user_id;
        std::string target_id;
        int target_type = 0;
        std::string emoji_id;
        int emoji_type = 0;
    };

    struct InboundMessage {
        InboundEventKind event_kind = InboundEventKind::message;
        std::string jid;
        std::string sender;
        std::string sender_name;
        std::string content;
        std::string timestamp;
        std::string message_id;
        std::string reference_message_index;
        std::string referenced_content;
        std::vector<Attachment> attachments;
        std::optional<ReactionInfo> reaction;
        bool mentioned = false;
        std::vector<std::string> mention_ids;
        bool is_group = false;
        std::string agent_override;
        std::string reply_target;
        bool isolated = false;
        bool light_context = false;

        [[nodiscard]]
        bool is_user_message() const {
            return event_kind == InboundEventKind::message;
        }
    };

    struct TextPayload {
        std::string text;
    };

    struct MarkdownPayload {
        std::string markdown;
    };

    struct MediaPayload {
        int file_type = 0;
        std::string url;
        std::string caption;
    };

    struct KeyboardPayload {
        std::string markdown;
        nlohmann::json keyboard_payload;
    };

    struct ArkPayload {
        nlohmann::json ark_payload;
    };

    struct EmbedPayload {
        nlohmann::json embed_payload;
    };

    using OutboundMessagePayload = std::variant<TextPayload, MarkdownPayload, MediaPayload, KeyboardPayload, ArkPayload, EmbedPayload>;

    struct OutboundMessage {
        OutboundMessagePayload payload;
        std::string reply_to_message_id;
        std::string reference_message_id;
    };

    using MessageCallback = std::function<void(const InboundMessage &)>;

    class Channel {
    public:
        Channel() = default;
        virtual ~Channel() = default;
        Channel(const Channel &) = delete;
        Channel &operator=(const Channel &) = delete;
        Channel(Channel &&) = delete;
        Channel &operator=(Channel &&) = delete;

        [[nodiscard]]
        virtual std::string name() const = 0;
        virtual void connect(MessageCallback on_message) = 0;
        virtual void send(const std::string &jid, const OutboundMessage &message) = 0;

        void send_message(const std::string &jid, const std::string &text, const std::string &reply_to_message_id = "") {
            send(jid, OutboundMessage{
                          .payload = TextPayload{.text = text},
                          .reply_to_message_id = reply_to_message_id,
                      });
        }

        void send_markdown_message(const std::string &jid, const std::string &markdown, const std::string &reply_to_message_id = "", const std::string &reference_message_id = "") {
            send(jid, OutboundMessage{
                          .payload = MarkdownPayload{.markdown = markdown},
                          .reply_to_message_id = reply_to_message_id,
                          .reference_message_id = reference_message_id,
                      });
        }

        void send_media_message(const std::string &jid, int file_type, const std::string &url, const std::string &reply_to_message_id = "", const std::string &caption = "",
                                const std::string &reference_message_id = "") {
            send(jid, OutboundMessage{
                          .payload =
                              MediaPayload{
                                  .file_type = file_type,
                                  .url = url,
                                  .caption = caption,
                              },
                          .reply_to_message_id = reply_to_message_id,
                          .reference_message_id = reference_message_id,
                      });
        }

        void send_keyboard_message(const std::string &jid, const std::string &markdown, const nlohmann::json &keyboard_payload, const std::string &reply_to_message_id = "",
                                   const std::string &reference_message_id = "") {
            send(jid, OutboundMessage{
                          .payload =
                              KeyboardPayload{
                                  .markdown = markdown,
                                  .keyboard_payload = keyboard_payload,
                              },
                          .reply_to_message_id = reply_to_message_id,
                          .reference_message_id = reference_message_id,
                      });
        }

        void send_ark_message(const std::string &jid, const nlohmann::json &ark_payload, const std::string &reply_to_message_id = "",
                              const std::string &reference_message_id = "") {
            send(jid, OutboundMessage{
                          .payload = ArkPayload{.ark_payload = ark_payload},
                          .reply_to_message_id = reply_to_message_id,
                          .reference_message_id = reference_message_id,
                      });
        }

        void send_embed_message(const std::string &jid, const nlohmann::json &embed_payload, const std::string &reply_to_message_id = "",
                                const std::string &reference_message_id = "") {
            send(jid, OutboundMessage{
                          .payload = EmbedPayload{.embed_payload = embed_payload},
                          .reply_to_message_id = reply_to_message_id,
                          .reference_message_id = reference_message_id,
                      });
        }

        virtual Attachment download_attachment(const std::string &jid, const Attachment &attachment, const std::string &destination_path) {
            static_cast<void>(jid);
            static_cast<void>(attachment);
            static_cast<void>(destination_path);
            throw std::runtime_error("Channel does not support attachment downloads");
        }

        virtual void add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
            static_cast<void>(jid);
            static_cast<void>(message_id);
            static_cast<void>(type);
            static_cast<void>(id);
            throw std::runtime_error("Channel does not support reactions");
        }
        virtual void remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
            static_cast<void>(jid);
            static_cast<void>(message_id);
            static_cast<void>(type);
            static_cast<void>(id);
            throw std::runtime_error("Channel does not support reactions");
        }
        virtual void disconnect() = 0;

        virtual void start_typing(const std::string &jid, const std::string &message_id) {
            static_cast<void>(jid);
            static_cast<void>(message_id);
        }

        virtual void stop_typing(const std::string &jid) {
            static_cast<void>(jid);
        }

        [[nodiscard]]
        virtual bool owns_jid(const std::string &jid) const = 0;

        [[nodiscard]]
        virtual bool is_connected() const = 0;

        [[nodiscard]]
        virtual std::vector<std::string> known_user_jids() const {
            return {};
        }
    };

    class ChannelManager {
    public:
        explicit ChannelManager(Allowlist allowlist = {});

        void add_channel(std::unique_ptr<Channel> ch);
        void connect_all(const MessageCallback &on_message);
        void send(const std::string &jid, const OutboundMessage &message);
        void send(const std::string &jid, const std::string &text, const std::string &reply_to_message_id = "");
        void send_markdown(const std::string &jid, const std::string &markdown, const std::string &reply_to_message_id = "", const std::string &reference_message_id = "");
        void send_media(const std::string &jid, int file_type, const std::string &url, const std::string &reply_to_message_id = "", const std::string &caption = "",
                        const std::string &reference_message_id = "");
        void send_keyboard(const std::string &jid, const std::string &markdown, const nlohmann::json &keyboard_payload, const std::string &reply_to_message_id = "",
                           const std::string &reference_message_id = "");
        void send_ark(const std::string &jid, const nlohmann::json &ark_payload, const std::string &reply_to_message_id = "", const std::string &reference_message_id = "");
        void send_embed(const std::string &jid, const nlohmann::json &embed_payload, const std::string &reply_to_message_id = "", const std::string &reference_message_id = "");
        [[nodiscard]]
        Attachment download_attachment(const std::string &jid, const Attachment &attachment, const std::string &destination_path);
        void add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id);
        void remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id);
        void start_typing(const std::string &jid, const std::string &message_id);
        void stop_typing(const std::string &jid);
        void disconnect_all();

        [[nodiscard]]
        bool has_channels() const;

        [[nodiscard]]
        std::vector<std::string> known_user_jids() const;

    private:
        std::vector<std::unique_ptr<Channel>> channels_;
        Allowlist allowlist_;
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::ArkPayload;
    using channel::Attachment;
    using channel::Channel;
    using channel::ChannelManager;
    using channel::EmbedPayload;
    using channel::InboundEventKind;
    using channel::InboundMessage;
    using channel::KeyboardPayload;
    using channel::MarkdownPayload;
    using channel::MediaPayload;
    using channel::MessageCallback;
    using channel::OutboundMessage;
    using channel::OutboundMessagePayload;
    using channel::ReactionInfo;
    using channel::TextPayload;

} // namespace orangutan
