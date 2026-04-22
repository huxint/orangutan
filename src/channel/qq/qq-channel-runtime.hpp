#pragma once

#include "channel/channel.hpp"
#include "channel/qq/qq-transport.hpp"
#include "types/base.hpp"
#include "utils/periodic-task.hpp"

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <exec/async_scope.hpp>

namespace orangutan::channel::qq {

    // QQ gateway opcodes as defined by the bot protocol.
    inline constexpr int GATEWAY_OP_DISPATCH = 0;
    inline constexpr int GATEWAY_OP_HEARTBEAT = 1;
    inline constexpr int GATEWAY_OP_IDENTIFY = 2;
    inline constexpr int GATEWAY_OP_RESUME = 6;
    inline constexpr int GATEWAY_OP_RECONNECT = 7;
    inline constexpr int GATEWAY_OP_INVALID_SESSION = 9;
    inline constexpr int GATEWAY_OP_HELLO = 10;
    inline constexpr int GATEWAY_OP_HEARTBEAT_ACK = 11;

    inline constexpr std::uint32_t INTENT_PUBLIC_GUILD_MESSAGES = 1U << 9;
    inline constexpr std::uint32_t INTENT_GUILD_MESSAGE_REACTIONS = 1U << 10;
    inline constexpr std::uint32_t INTENT_GROUP_MESSAGES = 1U << 25;
    inline constexpr std::uint32_t INTENT_INTERACTION = 1U << 26;
    inline constexpr std::uint32_t INTENT_GUILD_AT_MESSAGE = 1U << 30;
    inline constexpr std::uint32_t INTENT_DIRECT_MESSAGES = 1U << 12;

    struct qq_channel_runtime_state {
        struct known_user {
            std::string kind;
            std::string openid;
            std::chrono::system_clock::time_point last_seen_at;
        };

        struct typing_state {
            std::string message_id;
            std::chrono::steady_clock::time_point last_sent_at;
        };

        struct pending_outbound_send {
            std::string jid;
            OutboundMessage message;
        };

        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
        bool hello_received = false;
        bool close_requested = false;
        std::string last_error;
        std::uint32_t last_seq = 0;
        std::string session_id;
        std::chrono::milliseconds heartbeat_interval{0};
        utils::PeriodicTask heartbeat_task;
        utils::PeriodicTask token_refresh_task;
        std::mutex outbound_send_mutex;
        std::deque<pending_outbound_send> outbound_send_queue;
        std::shared_ptr<exec::async_scope> outbound_send_scope;
        bool outbound_send_draining = false;
        bool outbound_send_shutdown = false;
        std::mutex known_users_mutex;
        utils::PeriodicTask known_users_persist_task;
        std::unordered_map<std::string, known_user> known_users;
        bool known_users_dirty = false;
        std::mutex group_history_mutex;
        std::unordered_map<std::string, std::deque<std::string>> group_history;
        std::size_t group_history_limit = 50;
        std::mutex ref_index_mutex;
        std::unordered_map<std::string, std::string> ref_index_cache;
        static constexpr std::size_t REF_INDEX_MAX_ENTRIES = 50000;
        std::mutex typing_mutex;
        utils::PeriodicTask typing_task;
        std::unordered_map<std::string, typing_state> typing_states;
        std::unique_ptr<Transport> websocket;
    };

} // namespace orangutan::channel::qq
