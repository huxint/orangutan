#pragma once

#include "channel/channel.hpp"
#include "channel/qq/qq-api-client.hpp"
#include "channel/qq/qq-message-builder.hpp"
#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace orangutan::channel::qq {

    class QqChannel : public Channel {
    public:
        QqChannel(std::string bot_name, std::string app_id, std::string client_secret);
        ~QqChannel() override;
        QqChannel(const QqChannel &) = delete;
        QqChannel &operator=(const QqChannel &) = delete;
        QqChannel(QqChannel &&) = delete;
        QqChannel &operator=(QqChannel &&) = delete;

        [[nodiscard]]
        std::string name() const override;

        void connect(MessageCallback on_message) override;
        void send_message(const std::string &jid, const std::string &text, const std::string &reply_to_message_id = "") override;
        void send_markdown_message(const std::string &jid, const std::string &markdown, const std::string &reply_to_message_id = "", const std::string &reference_message_id = "");
        void send_media_message(const std::string &jid, int file_type, const std::string &url, const std::string &reply_to_message_id = "");
        void send_keyboard_message(const std::string &jid, const std::string &markdown, const nlohmann::json &keyboard_payload, const std::string &reply_to_message_id = "");
        void add_reaction(const std::string &channel_id, const std::string &message_id, const std::string &type, const std::string &id);
        void remove_reaction(const std::string &channel_id, const std::string &message_id, const std::string &type, const std::string &id);
        void disconnect() override;

        [[nodiscard]]
        bool owns_jid(const std::string &jid) const override;

        [[nodiscard]]
        bool is_connected() const override;
        [[nodiscard]]
        std::vector<std::string> known_user_jids() const;

    private:
        struct MessageReplyTracker {
            std::chrono::steady_clock::time_point received_at;
            int reply_count = 0;

            static constexpr int max_replies = 4;
            static constexpr auto ttl = std::chrono::hours(1);

            [[nodiscard]]
            bool can_reply(std::chrono::steady_clock::time_point now) const {
                return reply_count < max_replies && (now - received_at) < ttl;
            }
        };

        struct RuntimeState;

        std::string bot_name_;
        std::string app_id_;
        std::string client_secret_;
        std::unique_ptr<QqApiClient> api_client_;
        std::atomic<base::u16> msg_seq_{0};
        std::unordered_map<std::string, MessageReplyTracker> reply_trackers_;
        mutable std::mutex reply_trackers_mutex_;
        MessageCallback on_message_;
        std::atomic<bool> connected_{false};
        std::unique_ptr<RuntimeState> runtime_;

        void ensure_access_token();

        [[nodiscard]]
        std::string get_gateway_url();

        void connect_websocket(const std::string &gateway_url);
        void send_gateway_identity_or_resume();
        void send_gateway_payload(const nlohmann::json &payload);
        void send_message_now(const std::string &jid, const std::string &text, const std::string &reply_to_message_id);
        void restart_heartbeat(std::chrono::milliseconds interval);
        void stop_heartbeat();
        void start_token_refresh_loop();
        void stop_token_refresh_loop();
        void start_debounce_loop();
        void stop_debounce_loop();
        void enqueue_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id);
        void flush_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id);
        void load_session_state();
        void persist_session_state();
        void clear_session_state();
        void load_known_users();
        void persist_known_users();
        void remember_known_user(std::string_view kind, const std::string &openid);
        void remember_group_history(const std::string &jid, const std::string &sender_name, const std::string &content);
        [[nodiscard]]
        std::string consume_group_history(const std::string &jid);
        void handle_ws_message(const std::string &data);
        void handle_dispatch(const std::string &event_type, const nlohmann::json &data);
        void handle_c2c_message(const nlohmann::json &data);
        void handle_group_message(const nlohmann::json &data);
        void handle_guild_message(const nlohmann::json &data);
        void handle_interaction(const nlohmann::json &data);
        void send_c2c(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        void send_group(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        void send_guild(const std::string &channel_id, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        void send_markdown_c2c(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        void send_markdown_group(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        void send_markdown_guild(const std::string &channel_id, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id = "");
        [[nodiscard]]
        std::string upload_media_c2c(const std::string &openid, int file_type, const std::string &url);
        [[nodiscard]]
        std::string upload_media_group(const std::string &openid, int file_type, const std::string &url);
        void send_media_c2c(const std::string &openid, const std::string &file_info, const std::string &reply_to_message_id = "");
        void send_media_group(const std::string &openid, const std::string &file_info, const std::string &reply_to_message_id = "");
        void clear_ready_state();
        [[nodiscard]]
        base::u16 next_msg_seq();
        void remember_inbound_message(const std::string &message_id);
        [[nodiscard]]
        bool consume_passive_reply_quota(const std::string &message_id);
        [[nodiscard]]
        std::string resolve_passive_reply_message_id(const std::string &reply_to_message_id);

        [[nodiscard]]
        static std::vector<Attachment> parse_attachments(const nlohmann::json &data);

        [[nodiscard]]
        static std::vector<std::string> parse_mention_ids(const nlohmann::json &data);

        [[nodiscard]]
        bool is_bot_mentioned(const nlohmann::json &data, const std::vector<std::string> &mention_ids) const;

        [[nodiscard]]
        static std::string strip_mentions(const std::string &content);

        [[nodiscard]]
        static std::vector<std::string> chunk_text(const std::string &text, std::size_t limit);
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    using channel::qq::QqChannel;

} // namespace orangutan
