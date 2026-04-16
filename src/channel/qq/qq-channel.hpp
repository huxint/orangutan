#pragma once

#include "channel/channel.hpp"
#include "channel/qq/qq-api-client.hpp"
#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan::utils {
    class TaskPool;
}

namespace orangutan::channel::qq {
    [[nodiscard]]
    base::i64 parse_integer_like(const nlohmann::json &payload, std::string_view key, base::i64 default_value);

    struct qq_channel_runtime_state;

    class QqChannel : public Channel {
    public:
        QqChannel(std::string bot_name, std::string app_id, std::string client_secret, utils::TaskPool &task_pool);
        ~QqChannel() override;
        QqChannel(const QqChannel &) = delete;
        QqChannel &operator=(const QqChannel &) = delete;
        QqChannel(QqChannel &&) = delete;
        QqChannel &operator=(QqChannel &&) = delete;

        [[nodiscard]]
        std::string name() const override;

        void connect(MessageCallback on_message) override;
        void send(const std::string &jid, const OutboundMessage &message) override;
        [[nodiscard]]
        Attachment download_attachment(std::string_view jid, const Attachment &attachment, const std::filesystem::path &destination_path) override;
        void add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) override;
        void remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) override;
        void start_typing(const std::string &jid, const std::string &message_id) override;
        void stop_typing(const std::string &jid) override;
        void disconnect() override;

        [[nodiscard]]
        bool owns_jid(const std::string &jid) const override;

        [[nodiscard]]
        bool is_connected() const override;
        [[nodiscard]]
        std::vector<std::string> known_user_jids() const override;

    private:
        struct MessageReplyTracker {
            std::chrono::steady_clock::time_point received_at;
            int reply_count = 0;

            // NOLINTNEXTLINE(readability-identifier-naming)
            static constexpr int MAX_REPLIES = 4;
            // NOLINTNEXTLINE(readability-identifier-naming)
            static constexpr auto TTL = std::chrono::hours(1);

            [[nodiscard]]
            bool can_reply(std::chrono::steady_clock::time_point now) const {
                return reply_count < MAX_REPLIES && (now - received_at) < TTL;
            }
        };

        friend struct QqChannelTestAccess;

        std::string bot_name_;
        std::string app_id_;
        std::string client_secret_;
        utils::TaskPool *task_pool_ = nullptr;
        std::unique_ptr<QqApiClient> api_client_;
        std::atomic<base::u16> msg_seq_{0};
        std::unordered_map<std::string, MessageReplyTracker> reply_trackers_;
        mutable std::mutex reply_trackers_mutex_;
        MessageCallback on_message_;
        std::atomic<bool> connected_{false};
        std::unique_ptr<qq_channel_runtime_state> runtime_;

        void ensure_access_token();

        [[nodiscard]]
        std::string get_gateway_url();

        void connect_websocket(const std::string &gateway_url);
        void send_gateway_identity_or_resume();
        void send_gateway_payload(const nlohmann::json &payload);
        void restart_heartbeat(std::chrono::milliseconds interval);
        void stop_heartbeat();
        void start_token_refresh_loop();
        void stop_token_refresh_loop();
        void start_debounce_loop();
        void stop_debounce_loop();
        void enqueue_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id, const std::string &reference_message_id);
        void flush_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id, const std::string &reference_message_id);
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
        void handle_interaction(const nlohmann::json &data);
        void emit_inbound(const InboundMessage &message) const;
        void clear_ready_state();
        [[nodiscard]]
        base::u16 next_msg_seq();
        void remember_inbound_message(const std::string &message_id);
        [[nodiscard]]
        bool consume_passive_reply_quota(const std::string &message_id, int reply_units = 1);
        [[nodiscard]]
        std::string resolve_passive_reply_message_id(const std::string &reply_to_message_id, int reply_units = 1);

        void capture_outbound_ref_index(const QqApiResponse &response, const std::string &content);
        [[nodiscard]]
        std::string lookup_ref_index(const std::string &ref_idx) const;
        void load_ref_index();
        void append_ref_index_line(const std::string &ref_idx, const std::string &content);

        void send_typing_now(const std::string &jid, const std::string &message_id);
        void start_typing_keepalive();
        void stop_typing_keepalive();
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    using channel::qq::QqChannel;

} // namespace orangutan
