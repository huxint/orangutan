#pragma once

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

    inline constexpr base::u32 INTENT_PUBLIC_GUILD_MESSAGES = 1U << 9;
    inline constexpr base::u32 INTENT_GUILD_MESSAGE_REACTIONS = 1U << 10;
    inline constexpr base::u32 INTENT_GROUP_MESSAGES = 1U << 25;
    inline constexpr base::u32 INTENT_INTERACTION = 1U << 26;
    inline constexpr base::u32 INTENT_GUILD_AT_MESSAGE = 1U << 30;
    inline constexpr base::u32 INTENT_DIRECT_MESSAGES = 1U << 12;

    struct qq_channel_runtime_state {
        struct pending_debounced_message {
            std::string text;
            std::string reply_to_message_id;
            std::string reference_message_id;
            std::chrono::steady_clock::time_point first_enqueued_at;
            std::chrono::steady_clock::time_point last_update_at;
        };

        struct known_user {
            std::string kind;
            std::string openid;
            std::chrono::system_clock::time_point last_seen_at;
        };

        struct typing_state {
            std::string message_id;
            std::chrono::steady_clock::time_point last_sent_at;
        };

        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
        bool hello_received = false;
        bool close_requested = false;
        std::string last_error;
        base::u32 last_seq = 0;
        std::string session_id;
        std::chrono::milliseconds heartbeat_interval{0};
        utils::PeriodicTask heartbeat_task;
        utils::PeriodicTask token_refresh_task;
        utils::PeriodicTask debounce_task;
        std::mutex debounce_mutex;
        std::unordered_map<std::string, pending_debounced_message> pending_messages;
        std::chrono::milliseconds debounce_window{1500};
        std::chrono::milliseconds debounce_max_wait{8000};
        std::string debounce_separator = "\n\n---\n\n";
        std::mutex known_users_mutex;
        std::unordered_map<std::string, known_user> known_users;
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
