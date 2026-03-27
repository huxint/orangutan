#pragma once

#include "features/channel/core/channel.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace orangutan {

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
        void send_message(const std::string &jid, const std::string &text) override;
        void disconnect() override;

        [[nodiscard]]
        bool owns_jid(const std::string &jid) const override;

        [[nodiscard]]
        bool is_connected() const override;

    private:
        struct RuntimeState;

        std::string bot_name_;
        std::string app_id_;
        std::string client_secret_;
        std::string access_token_;
        std::chrono::steady_clock::time_point token_expiry_;
        MessageCallback on_message_;
        std::atomic<bool> connected_{false};
        std::unique_ptr<RuntimeState> runtime_;
        mutable std::mutex mutex_;

        void ensure_access_token();

        [[nodiscard]]
        std::string get_gateway_url();

        void connect_websocket(const std::string &gateway_url);
        void send_gateway_identity_or_resume();
        void send_gateway_payload(const nlohmann::json &payload);
        void restart_heartbeat(std::chrono::milliseconds interval);
        void stop_heartbeat();
        void handle_ws_message(const std::string &data);
        void handle_dispatch(const std::string &event_type, const nlohmann::json &data);
        void handle_c2c_message(const nlohmann::json &data);
        void handle_group_message(const nlohmann::json &data);
        void send_c2c(const std::string &openid, const std::string &content);
        void send_group(const std::string &openid, const std::string &content);
        void clear_ready_state();

        [[nodiscard]]
        static std::vector<std::string> chunk_text(const std::string &text, size_t limit);
    };

} // namespace orangutan
