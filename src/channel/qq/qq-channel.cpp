#include "channel/qq/qq-channel.hpp"

#include "channel/qq/qq-transport.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace orangutan::channel::qq {

    namespace {

        constexpr std::string_view qq_c2c_prefix = "qqbot:c2c:";
        constexpr std::string_view qq_group_prefix = "qqbot:group:";
        constexpr int gateway_op_dispatch = 0;
        constexpr int gateway_op_heartbeat = 1;
        constexpr int gateway_op_identify = 2;
        constexpr int gateway_op_resume = 6;
        constexpr int gateway_op_reconnect = 7;
        constexpr int gateway_op_invalid_session = 9;
        constexpr int gateway_op_hello = 10;
        constexpr int gateway_op_heartbeat_ack = 11;
        constexpr base::u32 intent_group_messages = 1U << 25;
        constexpr base::u32 intent_guild_at_message = 1U << 30;
        constexpr base::u32 intent_direct_messages = 1U << 12;
        constexpr std::chrono::seconds connect_timeout{10};

        std::string qq_jid_prefix(std::string_view bot_name, std::string_view kind) {
            if (bot_name.empty()) {
                return std::string("qqbot:") + std::string(kind) + ':';
            }
            return std::string("qqbot:") + std::string(bot_name) + ':' + std::string(kind) + ':';
        }

        std::string make_qq_jid(std::string_view bot_name, std::string_view kind, std::string_view openid) {
            return qq_jid_prefix(bot_name, kind) + std::string(openid);
        }

        std::string require_openid(std::string_view jid, std::string_view prefix) {
            if (!jid.starts_with(prefix)) {
                throw std::runtime_error("Unsupported QQ jid: " + std::string(jid));
            }
            return std::string(jid.substr(prefix.size()));
        }

        base::i64 parse_integer_like(const nlohmann::json &payload, std::string_view key, base::i64 default_value) {
            if (!payload.contains(key)) {
                return default_value;
            }

            const auto &value = payload.at(key);
            if (value.is_number_integer()) {
                return value.get<base::i64>();
            }
            if (value.is_string()) {
                const auto &str = value.get_ref<const std::string &>();
                base::i64 result = default_value;
                std::from_chars(str.data(), str.data() + str.size(), result);
                return result;
            }

            return default_value;
        }

        std::string getenv_or_default(const char *name, const char *fallback) {
            const char *value = std::getenv(name);
            if (value == nullptr || *value == '\0') {
                return fallback;
            }
            return value;
        }

    } // namespace

    struct QqChannel::RuntimeState {
        std::mutex mutex;
        std::condition_variable cv;
        std::mutex heartbeat_mutex;
        std::condition_variable heartbeat_cv;
        bool ready = false;
        bool hello_received = false;
        bool close_requested = false;
        std::string last_error;
        base::u32 last_seq = 0;
        std::string session_id;
        std::chrono::milliseconds heartbeat_interval{0};
        std::atomic<bool> heartbeat_stop{false};
        std::thread heartbeat_thread;

        std::unique_ptr<qq::Transport> websocket;
    };

    QqChannel::QqChannel(std::string bot_name, std::string app_id, std::string client_secret)
    : bot_name_(std::move(bot_name)),
      app_id_(std::move(app_id)),
      client_secret_(std::move(client_secret)),
      api_client_(std::make_unique<QqApiClient>(app_id_, client_secret_)),
      runtime_(std::make_unique<RuntimeState>()) {}

    QqChannel::~QqChannel() {
        disconnect();
    }

    std::string QqChannel::name() const {
        if (bot_name_.empty()) {
            return "qqbot";
        }
        return "qqbot:" + bot_name_;
    }

    void QqChannel::connect(MessageCallback on_message) {
        on_message_ = std::move(on_message);

#ifndef ORANGUTAN_ENABLE_QQ_CHANNEL
        throw std::runtime_error("QQ channel support was not compiled in; rebuild with -DORANGUTAN_ENABLE_QQ_CHANNEL=ON");
#else
        disconnect();
        clear_ready_state();

        ensure_access_token();
        const auto gateway_url = get_gateway_url();
        spdlog::info("Connecting QQ gateway: {}", gateway_url);
        connect_websocket(gateway_url);

        std::unique_lock<std::mutex> lock(runtime_->mutex);
        const bool ready = runtime_->cv.wait_for(lock, connect_timeout, [this] {
            return runtime_->ready || !runtime_->last_error.empty();
        });

        if (!ready) {
            lock.unlock();
            disconnect();
            throw std::runtime_error("Timed out waiting for QQ gateway READY event");
        }

        if (!runtime_->last_error.empty()) {
            const auto err = runtime_->last_error;
            lock.unlock();
            disconnect();
            throw std::runtime_error(err);
        }
#endif
    }

    void QqChannel::send_message(const std::string &jid, const std::string &text, const std::string &reply_to_message_id) {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");

        if (jid.starts_with(c2c_prefix)) {
            send_c2c(require_openid(jid, c2c_prefix), text, reply_to_message_id);
            return;
        }

        if (jid.starts_with(group_prefix)) {
            send_group(require_openid(jid, group_prefix), text, reply_to_message_id);
            return;
        }

        throw std::runtime_error("Unsupported QQ jid: " + jid);
    }

    void QqChannel::disconnect() {
        connected_ = false;
        stop_heartbeat();

        {
            std::scoped_lock lock(runtime_->mutex);
            runtime_->close_requested = true;
            runtime_->ready = false;
            runtime_->hello_received = false;
            runtime_->last_error.clear();
            runtime_->session_id.clear();
            runtime_->last_seq = 0;
        }
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        if (runtime_->websocket != nullptr) {
            runtime_->websocket->stop();
            runtime_->websocket.reset();
        }
#endif
    }

    bool QqChannel::owns_jid(const std::string &jid) const {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");
        return jid.starts_with(c2c_prefix) || jid.starts_with(group_prefix);
    }

    bool QqChannel::is_connected() const {
        return connected_.load();
    }

    void QqChannel::ensure_access_token() {
        api_client_->ensure_access_token();
    }

    std::string QqChannel::get_gateway_url() {
        return api_client_->get_gateway_url();
    }

    void QqChannel::connect_websocket(const std::string &gateway_url) {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        runtime_->websocket = std::make_unique<qq::Transport>(qq::Transport::Callbacks{
            .on_open =
                [this] {
                    spdlog::info("QQ WebSocket connected");
                },
            .on_text =
                [this](const std::string &text) {
                    try {
                        handle_ws_message(text);
                    } catch (const std::exception &e) {
                        spdlog::error("Failed to process QQ WebSocket message: {}", e.what());
                    }
                },
            .on_close =
                [this](base::u16 code, std::string reason) {
                    spdlog::warn("QQ WebSocket closed: {} {}", code, reason);
                    connected_ = false;
                    stop_heartbeat();

                    bool close_requested = false;
                    bool was_ready = false;
                    {
                        std::scoped_lock lock(runtime_->mutex);
                        close_requested = runtime_->close_requested;
                        was_ready = runtime_->ready;
                        runtime_->ready = false;
                        if (!close_requested && !was_ready && runtime_->last_error.empty()) {
                            runtime_->last_error = "QQ WebSocket closed before READY: " + reason;
                        }
                    }
                    runtime_->cv.notify_all();

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                    if (!close_requested && runtime_->websocket != nullptr) {
                        runtime_->websocket->request_reconnect();
                    }
#endif
                },
            .on_error =
                [this](std::string error) {
                    spdlog::error("QQ WebSocket error: {}", error);
                    connected_ = false;

                    bool close_requested = false;
                    bool was_ready = false;
                    {
                        std::scoped_lock lock(runtime_->mutex);
                        close_requested = runtime_->close_requested;
                        was_ready = runtime_->ready;
                        runtime_->ready = false;
                        if (!close_requested && !was_ready && runtime_->last_error.empty()) {
                            runtime_->last_error = std::move(error);
                        }
                    }
                    runtime_->cv.notify_all();

#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                    if (!close_requested && runtime_->websocket != nullptr) {
                        runtime_->websocket->request_reconnect();
                    }
#endif
                },
        });

        runtime_->websocket->start(gateway_url);
#endif
    }

    void QqChannel::send_gateway_identity_or_resume() {
        ensure_access_token();
        nlohmann::json payload;
        const auto token = std::string("QQBot ") + api_client_->access_token();

        {
            std::scoped_lock lock(runtime_->mutex);
            if (!runtime_->session_id.empty() && runtime_->last_seq > 0) {
                payload = {
                    {"op", gateway_op_resume},
                    {"d",
                     {
                         {"token", token},
                         {"session_id", runtime_->session_id},
                         {"seq", runtime_->last_seq},
                     }},
                };
            } else {
                payload = {
                    {"op", gateway_op_identify},
                    {"d",
                     {
                         {"token", token},
                         {"intents", intent_group_messages | intent_guild_at_message | intent_direct_messages},
                         {"shard", nlohmann::json::array({0, 1})},
                         {"properties",
                          {
                              {"$os", getenv_or_default("OSTYPE", "linux")},
                              {"$browser", "orangutan"},
                              {"$device", "orangutan"},
                          }},
                     }},
                };
            }
        }

        send_gateway_payload(payload);
    }

    void QqChannel::send_gateway_payload(const nlohmann::json &payload) {
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
        if (runtime_->websocket == nullptr) {
            return;
        }
        runtime_->websocket->send_text(payload.dump());
#else
        static_cast<void>(payload);
#endif
    }

    void QqChannel::restart_heartbeat(std::chrono::milliseconds interval) {
        stop_heartbeat();

        runtime_->heartbeat_interval = interval;
        runtime_->heartbeat_stop = false;
        runtime_->heartbeat_thread = std::thread([this] {
            while (!runtime_->heartbeat_stop.load()) {
                std::unique_lock<std::mutex> wait_lock(runtime_->heartbeat_mutex);
                const bool should_stop = runtime_->heartbeat_cv.wait_for(wait_lock, runtime_->heartbeat_interval, [this] {
                    return runtime_->heartbeat_stop.load();
                });
                wait_lock.unlock();

                if (should_stop || runtime_->heartbeat_stop.load()) {
                    break;
                }

                nlohmann::json heartbeat{
                    {"op", gateway_op_heartbeat},
                };

                {
                    std::scoped_lock lock(runtime_->mutex);
                    if (runtime_->last_seq == 0) {
                        heartbeat["d"] = nullptr;
                    } else {
                        heartbeat["d"] = runtime_->last_seq;
                    }
                }

                try {
                    send_gateway_payload(heartbeat);
                } catch (const std::exception &e) {
                    spdlog::warn("QQ heartbeat send failed: {}", e.what());
                    connected_ = false;
                    return;
                }
            }
        });
    }

    void QqChannel::stop_heartbeat() {
        runtime_->heartbeat_stop = true;
        runtime_->heartbeat_cv.notify_all();
        if (runtime_->heartbeat_thread.joinable()) {
            runtime_->heartbeat_thread.join();
        }
    }

    void QqChannel::handle_ws_message(const std::string &data) {
        const auto payload = nlohmann::json::parse(data);

        if (payload.contains("s") && !payload.at("s").is_null()) {
            std::scoped_lock lock(runtime_->mutex);
            runtime_->last_seq = payload.at("s").get<base::u32>();
        }

        const auto op = payload.value("op", -1);
        switch (op) {
            case gateway_op_hello: {
                const auto hello = payload.value("d", nlohmann::json::object());
                const auto interval = std::chrono::milliseconds(parse_integer_like(hello, "heartbeat_interval", 41250));
                {
                    std::scoped_lock lock(runtime_->mutex);
                    runtime_->hello_received = true;
                    runtime_->last_error.clear();
                }
                restart_heartbeat(interval);
                send_gateway_identity_or_resume();
                break;
            }
            case gateway_op_dispatch: {
                const auto event_type = payload.value("t", std::string{});
                const auto event_data = payload.value("d", nlohmann::json::object());
                if (event_type == "READY") {
                    std::scoped_lock lock(runtime_->mutex);
                    runtime_->session_id = event_data.value("session_id", std::string{});
                    runtime_->ready = true;
                    runtime_->last_error.clear();
                    connected_ = true;
                    runtime_->cv.notify_all();
                    spdlog::info("QQ gateway READY for bot '{}'", event_data.value("session_id", std::string{"unknown"}));
                } else if (event_type == "RESUMED") {
                    std::scoped_lock lock(runtime_->mutex);
                    runtime_->ready = true;
                    runtime_->last_error.clear();
                    connected_ = true;
                    runtime_->cv.notify_all();
                    spdlog::info("QQ gateway session resumed");
                }

                handle_dispatch(event_type, event_data);
                break;
            }
            case gateway_op_heartbeat_ack:
                spdlog::debug("QQ heartbeat acknowledged");
                break;
            case gateway_op_reconnect:
                spdlog::warn("QQ gateway requested reconnect");
                connected_ = false;
                stop_heartbeat();
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                if (runtime_->websocket != nullptr) {
                    runtime_->websocket->request_reconnect();
                }
#endif
                break;
            case gateway_op_invalid_session:
                spdlog::warn("QQ gateway reported invalid session");
                connected_ = false;
                stop_heartbeat();
                {
                    std::scoped_lock lock(runtime_->mutex);
                    runtime_->session_id.clear();
                    runtime_->last_seq = 0;
                    runtime_->ready = false;
                }
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                if (runtime_->websocket != nullptr) {
                    runtime_->websocket->request_reconnect();
                }
#endif
                break;
            default:
                spdlog::debug("Unhandled QQ gateway opcode: {}", op);
                break;
        }
    }

    void QqChannel::handle_dispatch(const std::string &event_type, const nlohmann::json &data) {
        if (event_type == "C2C_MESSAGE_CREATE") {
            handle_c2c_message(data);
            return;
        }

        if (event_type == "GROUP_AT_MESSAGE_CREATE" || event_type == "GROUP_MESSAGE_CREATE") {
            handle_group_message(data);
        }
    }

    void QqChannel::handle_c2c_message(const nlohmann::json &data) {
        if (on_message_ == nullptr) {
            return;
        }

        const auto author = data.value("author", nlohmann::json::object());
        const auto openid = author.value("user_openid", author.value("id", std::string{}));
        const auto sender_id = author.value("id", std::string{});
        const auto message_id = data.value("id", std::string{});
        remember_inbound_message(message_id);
        on_message_({
            .jid = make_qq_jid(bot_name_, "c2c", openid),
            .sender = sender_id,
            .sender_name = openid,
            .content = data.value("content", std::string{}),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .attachments = parse_attachments(data),
            .is_group = false,
        });
    }

    void QqChannel::handle_group_message(const nlohmann::json &data) {
        if (on_message_ == nullptr) {
            return;
        }

        const auto author = data.value("author", nlohmann::json::object());
        const auto group_openid = data.value("group_openid", std::string{});
        const auto message_id = data.value("id", std::string{});
        const auto mention_ids = parse_mention_ids(data);
        remember_inbound_message(message_id);
        on_message_({
            .jid = make_qq_jid(bot_name_, "group", group_openid),
            .sender = author.value("id", std::string{}),
            .sender_name = author.value("member_openid", author.value("id", std::string{})),
            .content = strip_mentions(data.value("content", std::string{})),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .attachments = parse_attachments(data),
            .mentioned = is_bot_mentioned(data, mention_ids),
            .mention_ids = mention_ids,
            .is_group = true,
        });
    }

    void QqChannel::send_c2c(const std::string &openid, const std::string &content, const std::string &reply_to_message_id) {
        std::string passive_reply_message_id;
        if (!reply_to_message_id.empty()) {
            if (consume_passive_reply_quota(reply_to_message_id)) {
                passive_reply_message_id = reply_to_message_id;
            } else {
                spdlog::warn("QQ passive reply quota exceeded or expired for message_id='{}', falling back to proactive send", reply_to_message_id);
            }
        }

        for (const auto &chunk : chunk_text(content, 4000)) {
            nlohmann::json payload = {
                {"content", chunk},
                {"msg_type", 0},
                {"msg_seq", next_msg_seq()},
            };
            if (!passive_reply_message_id.empty()) {
                payload["msg_id"] = passive_reply_message_id;
            }
            static_cast<void>(api_client_->post("/v2/users/" + openid + "/messages", payload));
        }
    }

    void QqChannel::send_group(const std::string &openid, const std::string &content, const std::string &reply_to_message_id) {
        std::string passive_reply_message_id;
        if (!reply_to_message_id.empty()) {
            if (consume_passive_reply_quota(reply_to_message_id)) {
                passive_reply_message_id = reply_to_message_id;
            } else {
                spdlog::warn("QQ passive reply quota exceeded or expired for message_id='{}', falling back to proactive send", reply_to_message_id);
            }
        }

        for (const auto &chunk : chunk_text(content, 4000)) {
            nlohmann::json payload = {
                {"content", chunk},
                {"msg_type", 0},
                {"msg_seq", next_msg_seq()},
            };
            if (!passive_reply_message_id.empty()) {
                payload["msg_id"] = passive_reply_message_id;
            }
            static_cast<void>(api_client_->post("/v2/groups/" + openid + "/messages", payload));
        }
    }

    void QqChannel::clear_ready_state() {
        std::scoped_lock lock(runtime_->mutex);
        runtime_->ready = false;
        runtime_->hello_received = false;
        runtime_->close_requested = false;
        runtime_->last_error.clear();
    }

    base::u16 QqChannel::next_msg_seq() {
        const auto current = msg_seq_.fetch_add(1, std::memory_order_relaxed);
        return static_cast<base::u16>((current + 1U) & 0xFFFFU);
    }

    void QqChannel::remember_inbound_message(const std::string &message_id) {
        if (message_id.empty()) {
            return;
        }

        std::scoped_lock lock(reply_trackers_mutex_);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(reply_trackers_, [now](const auto &entry) {
            return !entry.second.can_reply(now);
        });

        reply_trackers_[message_id] = MessageReplyTracker{
            .received_at = now,
            .reply_count = 0,
        };
    }

    bool QqChannel::consume_passive_reply_quota(const std::string &message_id) {
        if (message_id.empty()) {
            return false;
        }

        std::scoped_lock lock(reply_trackers_mutex_);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(reply_trackers_, [now](const auto &entry) {
            return !entry.second.can_reply(now);
        });

        auto [it, inserted] = reply_trackers_.try_emplace(message_id, MessageReplyTracker{
                                                                          .received_at = now,
                                                                          .reply_count = 0,
                                                                      });
        auto &tracker = it->second;
        if (!inserted && !tracker.can_reply(now)) {
            return false;
        }
        if (tracker.reply_count >= MessageReplyTracker::max_replies) {
            return false;
        }

        ++tracker.reply_count;
        return true;
    }

    std::vector<Attachment> QqChannel::parse_attachments(const nlohmann::json &data) {
        std::vector<Attachment> attachments;
        if (!data.contains("attachments") || !data.at("attachments").is_array()) {
            return attachments;
        }

        for (const auto &item : data.at("attachments")) {
            attachments.push_back(Attachment{
                .content_type = item.value("content_type", std::string{}),
                .url = item.value("url", std::string{}),
                .filename = item.value("filename", std::string{}),
                .width = static_cast<int>(parse_integer_like(item, "width", 0)),
                .height = static_cast<int>(parse_integer_like(item, "height", 0)),
                .size = static_cast<int>(parse_integer_like(item, "size", 0)),
            });
        }

        return attachments;
    }

    std::vector<std::string> QqChannel::parse_mention_ids(const nlohmann::json &data) {
        std::vector<std::string> ids;
        if (!data.contains("mentions") || !data.at("mentions").is_array()) {
            return ids;
        }

        for (const auto &mention : data.at("mentions")) {
            const auto mention_id = mention.value("member_openid", mention.value("user_openid", mention.value("id", std::string{})));
            if (!mention_id.empty()) {
                ids.push_back(mention_id);
            }
        }

        return ids;
    }

    bool QqChannel::is_bot_mentioned(const nlohmann::json &data, const std::vector<std::string> &mention_ids) const {
        if (mention_ids.empty()) {
            return false;
        }

        if (data.contains("mentions") && data.at("mentions").is_array()) {
            for (const auto &mention : data.at("mentions")) {
                if (mention.value("bot", false)) {
                    return true;
                }
                const auto mention_id = mention.value("id", std::string{});
                if (!mention_id.empty() && mention_id == app_id_) {
                    return true;
                }
            }
        }

        return data.value("content", std::string{}).contains("<@");
    }

    std::string QqChannel::strip_mentions(const std::string &content) {
        static const std::regex mention_pattern(R"(<@!?[^>]+>)");
        auto stripped = std::regex_replace(content, mention_pattern, "");

        while (!stripped.empty() && std::isspace(static_cast<unsigned char>(stripped.front())) != 0) {
            stripped.erase(stripped.begin());
        }
        while (!stripped.empty() && std::isspace(static_cast<unsigned char>(stripped.back())) != 0) {
            stripped.pop_back();
        }
        return stripped;
    }

    std::vector<std::string> QqChannel::chunk_text(const std::string &text, std::size_t limit) {
        if (text.empty() || text.size() <= limit) {
            return {text};
        }

        std::vector<std::string> chunks;
        std::string remaining = text;
        while (!remaining.empty()) {
            if (remaining.size() <= limit) {
                chunks.push_back(std::move(remaining));
                break;
            }

            std::size_t split_at = remaining.rfind('\n', limit);
            if (split_at == std::string::npos || split_at < limit / 2) {
                split_at = remaining.rfind(' ', limit);
            }
            if (split_at == std::string::npos || split_at < limit / 2) {
                split_at = limit;
            }

            chunks.push_back(remaining.substr(0, split_at));
            remaining.erase(0, split_at);
            remaining.erase(0, remaining.find_first_not_of(" \n\r\t"));
        }

        return chunks;
    }

} // namespace orangutan::channel::qq
