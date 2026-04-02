#pragma once

#include "channel/allowlist.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orangutan::channel {

    struct Attachment {
        std::string content_type;
        std::string url;
        std::string filename;
        int width = 0;
        int height = 0;
        int size = 0;
    };

    struct InboundMessage {
        std::string jid;
        std::string sender;
        std::string sender_name;
        std::string content;
        std::string timestamp;
        std::string message_id;
        std::vector<Attachment> attachments;
        bool mentioned = false;
        std::vector<std::string> mention_ids;
        bool is_group = false;
        std::string agent_override;
        std::string reply_target;
        bool isolated = false;
        bool light_context = false;
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
        virtual void send_message(const std::string &jid, const std::string &text, const std::string &reply_to_message_id = "") = 0;
        virtual void disconnect() = 0;

        [[nodiscard]]
        virtual bool owns_jid(const std::string &jid) const = 0;

        [[nodiscard]]
        virtual bool is_connected() const = 0;
    };

    class ChannelManager {
    public:
        explicit ChannelManager(Allowlist allowlist = {});

        void add_channel(std::unique_ptr<Channel> ch);
        void connect_all(const MessageCallback &on_message);
        void send(const std::string &jid, const std::string &text, const std::string &reply_to_message_id = "");
        void disconnect_all();

        [[nodiscard]]
        bool has_channels() const;

    private:
        std::vector<std::unique_ptr<Channel>> channels_;
        Allowlist allowlist_;
    };

} // namespace orangutan::channel

namespace orangutan {

    using channel::Attachment;
    using channel::Channel;
    using channel::ChannelManager;
    using channel::InboundMessage;
    using channel::MessageCallback;

} // namespace orangutan
