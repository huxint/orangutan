#include "channel/qq/qq-channel.hpp"

#include "channel/qq/qq-channel-inbound.hpp"
#include "channel/qq/qq-channel-outbound.hpp"
#include "channel/qq/qq-channel-runtime.hpp"
#include "channel/qq/qq-channel-session.hpp"
#include "channel/qq/qq-message-builder.hpp"
#include "channel/qq/qq-transport.hpp"
#include "utils/string.hpp"
#include "types/base.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/fmt/bundled/format.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace orangutan::channel::qq {

    namespace {
        constexpr int GATEWAY_OP_DISPATCH = 0;
        constexpr int GATEWAY_OP_HEARTBEAT = 1;
        constexpr int GATEWAY_OP_IDENTIFY = 2;
        constexpr int GATEWAY_OP_RESUME = 6;
        constexpr int GATEWAY_OP_RECONNECT = 7;
        constexpr int GATEWAY_OP_INVALID_SESSION = 9;
        constexpr int GATEWAY_OP_HELLO = 10;
        constexpr int GATEWAY_OP_HEARTBEAT_ACK = 11;
        constexpr base::u32 INTENT_PUBLIC_GUILD_MESSAGES = 1U << 9;
        constexpr base::u32 INTENT_GUILD_MESSAGE_REACTIONS = 1U << 10;
        constexpr base::u32 INTENT_GROUP_MESSAGES = 1U << 25;
        constexpr base::u32 INTENT_INTERACTION = 1U << 26;
        constexpr base::u32 INTENT_GUILD_AT_MESSAGE = 1U << 30;
        constexpr base::u32 INTENT_DIRECT_MESSAGES = 1U << 12;
        constexpr std::chrono::seconds CONNECT_TIMEOUT{10};

        template <class... Ts>
        struct Overloaded : Ts... {
            using Ts::operator()...;
        };

        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

    } // namespace

    using RuntimeState = qq_channel_runtime_state;

    QqChannel::QqChannel(std::string bot_name, std::string app_id, std::string client_secret)
    : bot_name_(std::move(bot_name)),
      app_id_(std::move(app_id)),
      client_secret_(std::move(client_secret)),
      api_client_(std::make_unique<QqApiClient>(app_id_, client_secret_)),
      runtime_(std::make_unique<qq_channel_runtime_state>()) {}

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
        load_session_state();
        load_known_users();
        load_ref_index();
        start_token_refresh_loop();
        start_debounce_loop();
        start_typing_keepalive();

        ensure_access_token();
        const auto gateway_url = get_gateway_url();
        spdlog::info("Connecting QQ gateway: {}", gateway_url);
        connect_websocket(gateway_url);

        std::unique_lock<std::mutex> lock(runtime_->mutex);
        const bool ready = runtime_->cv.wait_for(lock, CONNECT_TIMEOUT, [this] {
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

    void QqChannel::send(const std::string &jid, const OutboundMessage &message) {
        const auto send_immediately = [this, &jid](const OutboundMessage &outbound_message) {
            const auto target = resolve_send_target(bot_name_, jid);
            const auto post_payload = [this, &target](const nlohmann::json &payload) {
                const auto response = api_client_->post(message_path(target), payload);
                std::string content_text;
                if (payload.contains("markdown") && payload.at("markdown").contains("content")) {
                    content_text = payload.at("markdown").at("content").get<std::string>();
                } else if (payload.contains("content") && payload.at("content").is_string()) {
                    content_text = payload.at("content").get<std::string>();
                }
                if (!content_text.empty()) {
                    capture_outbound_ref_index(response, content_text);
                }
            };
            const auto upload_media_info = [this](const qq_send_target &upload_target, int file_type, const std::string &url) {
                const auto response = api_client_->post(media_upload_path(upload_target), nlohmann::json{{"file_type", file_type}, {"url", url}, {"srv_send_msg", false}});
                const auto upload_payload = response.parse_json_body();
                if (!upload_payload.contains("file_info") || !upload_payload.at("file_info").is_string()) {
                    throw std::runtime_error("QQ upload media missing file_info");
                }
                return upload_payload.at("file_info").get<std::string>();
            };

            route_outbound_payload(
                target, outbound_message, post_payload, upload_media_info,
                [this] {
                    return next_msg_seq();
                },
                [this](const std::string &reply_to_message_id, int reply_units) {
                    return resolve_passive_reply_message_id(reply_to_message_id, reply_units);
                });
        };

        if (const auto *text = std::get_if<TextPayload>(&message.payload)) {
            if (text_contains_media_markup(text->text)) {
                route_media_segments(jid, message, send_immediately,
                                     [this, &jid](const std::string &text_value, const std::string &reply_to_message_id, const std::string &reference_message_id) {
                                         enqueue_debounced_text(jid, text_value, reply_to_message_id, reference_message_id);
                                     });
                return;
            }
            enqueue_debounced_text(jid, text->text, message.reply_to_message_id, message.reference_message_id);
            return;
        }

        send_immediately(message);
    }

    Attachment QqChannel::download_attachment(std::string_view jid, const Attachment &attachment, const std::filesystem::path &destination_path) {
        static_cast<void>(resolve_send_target(bot_name_, jid));

        auto downloaded = attachment;
        downloaded.download_pending = false;
        downloaded.local_path.clear();
        downloaded.download_error.clear();

        if (attachment.url.empty()) {
            downloaded.download_error = "attachment has no downloadable url";
            return downloaded;
        }

        const auto response = api_client_->get(attachment.url);
        if (destination_path.empty()) {
            throw std::runtime_error("attachment destination path must not be empty");
        }
        if (!destination_path.parent_path().empty()) {
            std::filesystem::create_directories(destination_path.parent_path());
        }

        std::ofstream output(destination_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("unable to open attachment destination for write");
        }
        output.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        output.close();
        downloaded.local_path = destination_path.string();
        return downloaded;
    }

    void QqChannel::add_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");
        if (!jid.starts_with(guild_prefix)) {
            throw std::runtime_error("QQ reactions are only supported for guild channel jids");
        }
        const auto channel_id = require_openid(jid, guild_prefix);
        static_cast<void>(api_client_->put("/channels/" + channel_id + "/messages/" + message_id + "/reactions/" + type + "/" + id, nlohmann::json::object()));
    }

    void QqChannel::remove_reaction(const std::string &jid, const std::string &message_id, const std::string &type, const std::string &id) {
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");
        if (!jid.starts_with(guild_prefix)) {
            throw std::runtime_error("QQ reactions are only supported for guild channel jids");
        }
        const auto channel_id = require_openid(jid, guild_prefix);
        static_cast<void>(api_client_->del("/channels/" + channel_id + "/messages/" + message_id + "/reactions/" + type + "/" + id));
    }

    void QqChannel::disconnect() {
        connected_ = false;
        stop_heartbeat();
        stop_token_refresh_loop();
        stop_debounce_loop();
        stop_typing_keepalive();
        persist_known_users();

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
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");
        return jid.starts_with(c2c_prefix) || jid.starts_with(group_prefix) || jid.starts_with(guild_prefix);
    }

    bool QqChannel::is_connected() const {
        return connected_.load();
    }

    std::vector<std::string> QqChannel::known_user_jids() const {
        std::vector<std::string> jids;
        {
            std::scoped_lock lock(runtime_->known_users_mutex);
            jids.reserve(runtime_->known_users.size());
            for (const auto &[key, user] : runtime_->known_users) {
                static_cast<void>(key);
                jids.push_back(make_qq_jid(bot_name_, user.kind, user.openid));
            }
        }
        std::ranges::sort(jids);
        return jids;
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
                [] {
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

                    if (!close_requested && runtime_->websocket != nullptr) {
                        runtime_->websocket->request_reconnect();
                    }
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

                    if (!close_requested && runtime_->websocket != nullptr) {
                        runtime_->websocket->request_reconnect();
                    }
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
                    {"op", GATEWAY_OP_RESUME},
                    {"d",
                     {
                         {"token", token},
                         {"session_id", runtime_->session_id},
                         {"seq", runtime_->last_seq},
                     }},
                };
            } else {
                payload = {
                    {"op", GATEWAY_OP_IDENTIFY},
                    {"d",
                     {
                         {"token", token},
                         {"intents", INTENT_PUBLIC_GUILD_MESSAGES | INTENT_GUILD_MESSAGE_REACTIONS | INTENT_DIRECT_MESSAGES | INTENT_GROUP_MESSAGES | INTENT_INTERACTION |
                                         INTENT_GUILD_AT_MESSAGE},
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
        runtime_->heartbeat_thread = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested()) {
                std::unique_lock<std::mutex> wait_lock(runtime_->heartbeat_mutex);
                const bool should_stop = runtime_->heartbeat_cv.wait_for(wait_lock, token, runtime_->heartbeat_interval, [&token] {
                    return token.stop_requested();
                });
                wait_lock.unlock();

                if (should_stop || token.stop_requested()) {
                    break;
                }

                nlohmann::json heartbeat{
                    {"op", GATEWAY_OP_HEARTBEAT},
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
        if (runtime_->heartbeat_thread.joinable()) {
            runtime_->heartbeat_thread.request_stop();
            runtime_->heartbeat_cv.notify_all();
            runtime_->heartbeat_thread.join();
        }
    }

    void QqChannel::start_token_refresh_loop() {
        stop_token_refresh_loop();

        runtime_->token_refresh_thread = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested()) {
                std::unique_lock lock(runtime_->token_refresh_mutex);
                const bool should_stop = runtime_->token_refresh_cv.wait_for(lock, token, std::chrono::minutes(1), [&token] {
                    return token.stop_requested();
                });
                lock.unlock();

                if (should_stop || token.stop_requested()) {
                    break;
                }

                try {
                    api_client_->refresh_access_token_if_due();
                } catch (const std::exception &e) {
                    spdlog::warn("QQ background token refresh failed: {}", e.what());
                }
            }
        });
    }

    void QqChannel::stop_token_refresh_loop() {
        if (runtime_->token_refresh_thread.joinable()) {
            runtime_->token_refresh_thread.request_stop();
            runtime_->token_refresh_cv.notify_all();
            runtime_->token_refresh_thread.join();
        }
    }

    void QqChannel::start_debounce_loop() {
        stop_debounce_loop();

        runtime_->debounce_thread = std::jthread([this](std::stop_token token) {
            while (!token.stop_requested()) {
                std::vector<std::tuple<std::string, std::string, std::string, std::string>> ready_messages;
                {
                    std::unique_lock lock(runtime_->debounce_mutex);
                    runtime_->debounce_cv.wait_for(lock, token, std::chrono::milliseconds(200), [this, &token] {
                        return token.stop_requested() || !runtime_->pending_messages.empty();
                    });
                    if (token.stop_requested()) {
                        break;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    for (auto it = runtime_->pending_messages.begin(); it != runtime_->pending_messages.end();) {
                        const auto elapsed_since_update = now - it->second.last_update_at;
                        const auto elapsed_since_first = now - it->second.first_enqueued_at;
                        if (elapsed_since_update >= runtime_->debounce_window || elapsed_since_first >= runtime_->debounce_max_wait) {
                            ready_messages.emplace_back(it->first, it->second.text, it->second.reply_to_message_id, it->second.reference_message_id);
                            it = runtime_->pending_messages.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                for (const auto &[jid, text, reply_to, reference] : ready_messages) {
                    flush_debounced_text(jid, text, reply_to, reference);
                }
            }
        });
    }

    void QqChannel::stop_debounce_loop() {
        if (runtime_->debounce_thread.joinable()) {
            runtime_->debounce_thread.request_stop();
            runtime_->debounce_cv.notify_all();
            runtime_->debounce_thread.join();
        }

        std::vector<std::tuple<std::string, std::string, std::string, std::string>> remaining_messages;
        {
            std::scoped_lock lock(runtime_->debounce_mutex);
            for (const auto &[jid, pending] : runtime_->pending_messages) {
                remaining_messages.emplace_back(jid, pending.text, pending.reply_to_message_id, pending.reference_message_id);
            }
            runtime_->pending_messages.clear();
        }
        if (!connected_.load()) {
            return;
        }
        for (const auto &[jid, text, reply_to, reference] : remaining_messages) {
            flush_debounced_text(jid, text, reply_to, reference);
        }
    }

    void QqChannel::enqueue_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        if (text.empty()) {
            return;
        }

        std::optional<std::tuple<std::string, std::string, std::string, std::string>> immediate_flush;
        {
            std::scoped_lock lock(runtime_->debounce_mutex);
            const auto now = std::chrono::steady_clock::now();
            auto it = runtime_->pending_messages.find(jid);
            if (it != runtime_->pending_messages.end() && (it->second.reply_to_message_id != reply_to_message_id || it->second.reference_message_id != reference_message_id)) {
                immediate_flush.emplace(jid, it->second.text, it->second.reply_to_message_id, it->second.reference_message_id);
                runtime_->pending_messages.erase(it);
                it = runtime_->pending_messages.end();
            }

            if (it == runtime_->pending_messages.end()) {
                runtime_->pending_messages[jid] = RuntimeState::pending_debounced_message{
                    .text = text,
                    .reply_to_message_id = reply_to_message_id,
                    .reference_message_id = reference_message_id,
                    .first_enqueued_at = now,
                    .last_update_at = now,
                };
            } else {
                it->second.text.append(runtime_->debounce_separator);
                it->second.text.append(text);
                it->second.last_update_at = now;
            }
        }

        if (immediate_flush.has_value()) {
            const auto &[flush_jid, flush_text, flush_reply_to, flush_reference] = *immediate_flush;
            flush_debounced_text(flush_jid, flush_text, flush_reply_to, flush_reference);
        }
        runtime_->debounce_cv.notify_all();
    }

    void QqChannel::flush_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        try {
            const auto target = resolve_send_target(bot_name_, jid);
            const auto post_payload = [this, &target](const nlohmann::json &payload) {
                const auto response = api_client_->post(message_path(target), payload);
                std::string content_text;
                if (payload.contains("markdown") && payload.at("markdown").contains("content")) {
                    content_text = payload.at("markdown").at("content").get<std::string>();
                } else if (payload.contains("content") && payload.at("content").is_string()) {
                    content_text = payload.at("content").get<std::string>();
                }
                if (!content_text.empty()) {
                    capture_outbound_ref_index(response, content_text);
                }
            };
            const auto upload_media_info = [this](const qq_send_target &upload_target, int file_type, const std::string &url) {
                const auto response = api_client_->post(media_upload_path(upload_target), nlohmann::json{{"file_type", file_type}, {"url", url}, {"srv_send_msg", false}});
                const auto upload_payload = response.parse_json_body();
                if (!upload_payload.contains("file_info") || !upload_payload.at("file_info").is_string()) {
                    throw std::runtime_error("QQ upload media missing file_info");
                }
                return upload_payload.at("file_info").get<std::string>();
            };

            route_outbound_payload(
                target,
                OutboundMessage{
                    .payload = TextPayload{.text = text},
                    .reply_to_message_id = reply_to_message_id,
                    .reference_message_id = reference_message_id,
                },
                post_payload, upload_media_info,
                [this] {
                    return next_msg_seq();
                },
                [this](const std::string &reply_id, int reply_units) {
                    return resolve_passive_reply_message_id(reply_id, reply_units);
                });
        } catch (const std::exception &e) {
            spdlog::error("QQ debounced send failed for jid '{}': {}", jid, e.what());
        }
    }

    void QqChannel::load_session_state() {
        const auto file_path = qq_session_file_path(bot_name_);
        std::ifstream input(file_path);
        if (!input.is_open()) {
            return;
        }

        try {
            nlohmann::json payload;
            input >> payload;

            if (payload.value("app_id", std::string{}) != app_id_) {
                return;
            }
            if (!payload.contains("session_id") || !payload.contains("last_seq")) {
                return;
            }
            const auto saved_at_text = payload.value("saved_at", std::string{});
            const auto saved_at = parse_iso_utc(saved_at_text);
            if (!saved_at.has_value()) {
                return;
            }

            const auto age = std::chrono::system_clock::now() - *saved_at;
            if (age > std::chrono::minutes(5)) {
                return;
            }

            std::scoped_lock lock(runtime_->mutex);
            runtime_->session_id = payload.value("session_id", std::string{});
            runtime_->last_seq = payload.value("last_seq", 0U);
        } catch (const std::exception &e) {
            spdlog::warn("Failed to load QQ session state: {}", e.what());
        }
    }

    void QqChannel::persist_session_state() {
        std::string session_id;
        base::u32 last_seq = 0;
        {
            std::scoped_lock lock(runtime_->mutex);
            if (runtime_->session_id.empty() || runtime_->last_seq == 0) {
                return;
            }
            session_id = runtime_->session_id;
            last_seq = runtime_->last_seq;
        }

        const auto file_path = qq_session_file_path(bot_name_);
        try {
            std::filesystem::create_directories(file_path.parent_path());
            std::ofstream output(file_path, std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error("unable to open session file for write");
            }

            const nlohmann::json payload = {
                {"session_id", session_id},
                {"last_seq", last_seq},
                {"app_id", app_id_},
                {"saved_at", format_iso_utc(std::chrono::system_clock::now())},
            };
            output << payload.dump(2);
        } catch (const std::exception &e) {
            spdlog::warn("Failed to persist QQ session state: {}", e.what());
        }
    }

    void QqChannel::clear_session_state() {
        const auto file_path = qq_session_file_path(bot_name_);
        std::error_code error;
        std::filesystem::remove(file_path, error);
        if (error) {
            spdlog::warn("Failed to clear QQ session state '{}': {}", file_path.string(), error.message());
        }
    }

    void QqChannel::load_known_users() {
        const auto file_path = qq_known_users_file_path(bot_name_);
        std::ifstream input(file_path);
        if (!input.is_open()) {
            return;
        }

        try {
            nlohmann::json payload;
            input >> payload;
            if (!payload.is_array()) {
                return;
            }

            std::unordered_map<std::string, RuntimeState::known_user> loaded;
            for (const auto &item : payload) {
                const auto kind = item.value("kind", std::string{});
                const auto openid = item.value("openid", std::string{});
                if (kind.empty() || openid.empty()) {
                    continue;
                }

                const auto last_seen_text = item.value("last_seen", std::string{});
                const auto last_seen = parse_iso_utc(last_seen_text).value_or(std::chrono::system_clock::now());
                loaded.emplace(spdlog::fmt_lib::format("{}:{}", kind, openid), RuntimeState::known_user{
                                                                                   .kind = kind,
                                                                                   .openid = openid,
                                                                                   .last_seen_at = last_seen,
                                                                               });
            }

            std::scoped_lock lock(runtime_->known_users_mutex);
            runtime_->known_users = std::move(loaded);
        } catch (const std::exception &e) {
            spdlog::warn("Failed to load QQ known users: {}", e.what());
        }
    }

    void QqChannel::persist_known_users() {
        std::vector<RuntimeState::known_user> users;
        {
            std::scoped_lock lock(runtime_->known_users_mutex);
            users.reserve(runtime_->known_users.size());
            for (const auto &[key, user] : runtime_->known_users) {
                static_cast<void>(key);
                users.push_back(user);
            }
        }

        std::ranges::sort(users, [](const auto &lhs, const auto &rhs) {
            return std::tie(lhs.kind, lhs.openid) < std::tie(rhs.kind, rhs.openid);
        });

        const auto file_path = qq_known_users_file_path(bot_name_);
        try {
            nlohmann::json payload = nlohmann::json::array();
            for (const auto &user : users) {
                payload.push_back({
                    {"kind", user.kind},
                    {"openid", user.openid},
                    {"last_seen", format_iso_utc(user.last_seen_at)},
                });
            }

            std::filesystem::create_directories(file_path.parent_path());
            std::ofstream output(file_path, std::ios::trunc);
            if (!output.is_open()) {
                throw std::runtime_error("unable to open known users file for write");
            }
            output << payload.dump(2);
        } catch (const std::exception &e) {
            spdlog::warn("Failed to persist QQ known users: {}", e.what());
        }
    }

    void QqChannel::remember_known_user(std::string_view kind, const std::string &openid) {
        if (openid.empty()) {
            return;
        }

        const auto now = std::chrono::system_clock::now();
        {
            std::scoped_lock lock(runtime_->known_users_mutex);
            auto &entry = runtime_->known_users[std::string(kind) + ":" + openid];
            entry.kind = std::string(kind);
            entry.openid = openid;
            entry.last_seen_at = now;
        }
        persist_known_users();
    }

    void QqChannel::remember_group_history(const std::string &jid, const std::string &sender_name, const std::string &content) {
        if (jid.empty()) {
            return;
        }

        std::string line;
        if (sender_name.empty()) {
            line = content;
        } else if (content.empty()) {
            line = "[" + sender_name + "]";
        } else {
            line = "[" + sender_name + "] " + content;
        }
        if (line.empty()) {
            return;
        }

        std::scoped_lock lock(runtime_->group_history_mutex);
        auto &history = runtime_->group_history[jid];
        history.push_back(std::move(line));
        while (history.size() > runtime_->group_history_limit) {
            history.pop_front();
        }
    }

    std::string QqChannel::consume_group_history(const std::string &jid) {
        std::deque<std::string> history;
        {
            std::scoped_lock lock(runtime_->group_history_mutex);
            auto it = runtime_->group_history.find(jid);
            if (it == runtime_->group_history.end() || it->second.empty()) {
                return {};
            }
            auto history_node = runtime_->group_history.extract(it);
            history = std::move(history_node.mapped());
        }

        std::string merged;
        for (const auto &line : history) {
            if (!merged.empty()) {
                merged.push_back('\n');
            }
            merged.append(line);
        }
        return merged;
    }

    void QqChannel::handle_ws_message(const std::string &data) {
        const auto payload = nlohmann::json::parse(data);

        if (payload.contains("s") && !payload.at("s").is_null()) {
            {
                std::scoped_lock lock(runtime_->mutex);
                runtime_->last_seq = payload.at("s").get<base::u32>();
            }
            persist_session_state();
        }

        const auto op = payload.value("op", -1);
        switch (op) {
            case GATEWAY_OP_HELLO: {
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
            case GATEWAY_OP_DISPATCH: {
                const auto event_type = payload.value("t", std::string{});
                const auto event_data = payload.value("d", nlohmann::json::object());
                bool should_persist = false;
                if (event_type == "READY") {
                    {
                        std::scoped_lock lock(runtime_->mutex);
                        runtime_->session_id = event_data.value("session_id", std::string{});
                        runtime_->ready = true;
                        runtime_->last_error.clear();
                        connected_ = true;
                        runtime_->cv.notify_all();
                    }
                    should_persist = true;
                    spdlog::info("QQ gateway READY for bot '{}'", event_data.value("session_id", std::string{"unknown"}));
                } else if (event_type == "RESUMED") {
                    {
                        std::scoped_lock lock(runtime_->mutex);
                        runtime_->ready = true;
                        runtime_->last_error.clear();
                        connected_ = true;
                        runtime_->cv.notify_all();
                    }
                    should_persist = true;
                    spdlog::info("QQ gateway session resumed");
                }

                if (should_persist) {
                    persist_session_state();
                }

                handle_dispatch(event_type, event_data);
                break;
            }
            case GATEWAY_OP_HEARTBEAT_ACK:
                spdlog::debug("QQ heartbeat acknowledged");
                break;
            case GATEWAY_OP_RECONNECT:
                spdlog::warn("QQ gateway requested reconnect");
                connected_ = false;
                stop_heartbeat();
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                if (runtime_->websocket != nullptr) {
                    runtime_->websocket->request_reconnect();
                }
#endif
                break;
            case GATEWAY_OP_INVALID_SESSION:
                spdlog::warn("QQ gateway reported invalid session");
                connected_ = false;
                stop_heartbeat();
                {
                    std::scoped_lock lock(runtime_->mutex);
                    runtime_->session_id.clear();
                    runtime_->last_seq = 0;
                    runtime_->ready = false;
                }
                clear_session_state();
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
            if (on_message_ == nullptr) {
                return;
            }
            const auto author = data.value("author", nlohmann::json::object());
            const auto openid = author.value("user_openid", author.value("id", std::string{}));
            const auto sender_id = author.value("id", std::string{});
            const auto message_id = data.value("id", std::string{});
            remember_inbound_message(message_id);
            remember_known_user("c2c", openid);
            emit_inbound(make_c2c_inbound_message(bot_name_, openid, sender_id, data, lookup_ref_index(parse_message_scene_ext_value(data, "ref_msg_idx"))));
            return;
        }

        if (event_type == "GROUP_AT_MESSAGE_CREATE" || event_type == "GROUP_MESSAGE_CREATE") {
            if (on_message_ == nullptr) {
                return;
            }
            const auto author = data.value("author", nlohmann::json::object());
            const auto group_openid = data.value("group_openid", std::string{});
            const auto message_id = data.value("id", std::string{});
            const auto mention_ids = parse_mention_ids(data);
            const auto sender_openid = author.value("member_openid", author.value("user_openid", author.value("id", std::string{})));
            const auto sender_name = author.value("member_openid", author.value("id", std::string{}));
            const auto message_jid = make_qq_jid(bot_name_, "group", group_openid);
            const auto content = strip_mentions(data.value("content", std::string{}));
            const auto mentioned = is_bot_mentioned(app_id_, data, mention_ids);

            remember_inbound_message(message_id);
            remember_known_user("group", sender_openid);
            remember_group_history(message_jid, sender_name, content);
            if (!mentioned) {
                return;
            }

            auto aggregated_content = consume_group_history(message_jid);
            if (aggregated_content.empty()) {
                aggregated_content = content;
            }

            emit_inbound(make_group_inbound_message(bot_name_, group_openid, author.value("id", std::string{}), sender_name, data, aggregated_content,
                                                    lookup_ref_index(parse_message_scene_ext_value(data, "ref_msg_idx")), mention_ids, mentioned));
            return;
        }

        if (event_type == "AT_MESSAGE_CREATE" || event_type == "GUILD_MESSAGE_CREATE") {
            if (on_message_ == nullptr) {
                return;
            }
            const auto author = data.value("author", nlohmann::json::object());
            const auto channel_id = data.value("channel_id", std::string{});
            const auto message_id = data.value("id", std::string{});
            const auto mention_ids = parse_mention_ids(data);
            const auto sender_id = author.value("id", std::string{});
            remember_inbound_message(message_id);
            emit_inbound(make_guild_inbound_message(bot_name_, channel_id, sender_id, author.value("username", sender_id), data,
                                                    lookup_ref_index(parse_message_scene_ext_value(data, "ref_msg_idx")), mention_ids,
                                                    is_bot_mentioned(app_id_, data, mention_ids)));
            return;
        }

        if (event_type == "INTERACTION_CREATE") {
            handle_interaction(data);
            return;
        }

        if (event_type == "MESSAGE_REACTION_ADD" || event_type == "MESSAGE_REACTION_REMOVE") {
            if (on_message_ != nullptr) {
                emit_inbound(make_reaction_inbound_message(bot_name_, event_type, data));
            }
        }
    }

    void QqChannel::handle_interaction(const nlohmann::json &data) {
        const auto interaction_id = data.value("id", std::string{});
        if (!interaction_id.empty()) {
            try {
                static_cast<void>(api_client_->put("/interactions/" + interaction_id, nlohmann::json{
                                                                                          {"code", 0},
                                                                                      }));
            } catch (const std::exception &e) {
                spdlog::warn("QQ interaction ACK failed for '{}': {}", interaction_id, e.what());
            }
        }

        if (on_message_ == nullptr) {
            return;
        }

        const auto event_data = data.value("data", nlohmann::json::object());
        std::string button_data;
        if (data.contains("data")) {
            const auto resolved = event_data.value("resolved", nlohmann::json::object());
            button_data = resolved.value("button_data", std::string{});
            if (button_data.empty()) {
                button_data = event_data.value("button_data", std::string{});
            }
        }

        const auto user_openid = data.value("user_openid", std::string{});
        const auto group_openid = data.value("group_openid", event_data.value("group_openid", std::string{}));
        const auto channel_id = data.value("channel_id", std::string{});
        const bool is_guild = !channel_id.empty();
        const bool is_group = !is_guild && !group_openid.empty();
        if (!is_guild && !is_group) {
            remember_known_user("c2c", user_openid);
        } else if (is_group && !user_openid.empty()) {
            remember_known_user("group", user_openid);
        }
        emit_inbound({
            .jid = is_guild ? make_qq_jid(bot_name_, "guild", channel_id) : (is_group ? make_qq_jid(bot_name_, "group", group_openid) : make_qq_jid(bot_name_, "c2c", user_openid)),
            .sender = user_openid,
            .sender_name = user_openid,
            .content = button_data,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = interaction_id,
            .is_group = is_guild || is_group,
        });
    }

    void QqChannel::emit_inbound(const InboundMessage &message) const {
        if (on_message_ != nullptr) {
            on_message_(message);
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

    bool QqChannel::consume_passive_reply_quota(const std::string &message_id, int reply_units) {
        if (message_id.empty() || reply_units <= 0) {
            return false;
        }

        std::scoped_lock lock(reply_trackers_mutex_);
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(reply_trackers_, [now](const auto &entry) {
            return !entry.second.can_reply(now);
        });

        const auto it = reply_trackers_.find(message_id);
        if (it == reply_trackers_.end()) {
            return false;
        }

        auto &tracker = it->second;
        if (!tracker.can_reply(now)) {
            return false;
        }
        const auto remaining = MessageReplyTracker::MAX_REPLIES - tracker.reply_count;
        if (reply_units > remaining) {
            return false;
        }

        tracker.reply_count += reply_units;
        return true;
    }

    std::string QqChannel::resolve_passive_reply_message_id(const std::string &reply_to_message_id, int reply_units) {
        if (reply_to_message_id.empty()) {
            return {};
        }
        if (consume_passive_reply_quota(reply_to_message_id, reply_units)) {
            return reply_to_message_id;
        }
        spdlog::warn("QQ passive reply quota exceeded or expired for message_id='{}', falling back to proactive send", reply_to_message_id);
        return {};
    }

    // ── Ref Index Store ────────────────────────────────────────────────

    void QqChannel::capture_outbound_ref_index(const QqApiResponse &response, const std::string &content) {
        try {
            const auto body = response.parse_json_body();
            if (!body.contains("ext_info")) {
                return;
            }
            const auto &ext_info = body.at("ext_info");
            if (!ext_info.contains("ref_idx") || !ext_info.at("ref_idx").is_string()) {
                return;
            }
            const auto ref_idx = ext_info.at("ref_idx").get<std::string>();
            if (ref_idx.empty()) {
                return;
            }

            {
                std::scoped_lock lock(runtime_->ref_index_mutex);
                if (runtime_->ref_index_cache.size() >= RuntimeState::REF_INDEX_MAX_ENTRIES) {
                    auto it = runtime_->ref_index_cache.begin();
                    if (it != runtime_->ref_index_cache.end()) {
                        runtime_->ref_index_cache.erase(it);
                    }
                }
                runtime_->ref_index_cache[ref_idx] = content;
            }
            append_ref_index_line(ref_idx, content);
        } catch (const std::exception &e) {
            spdlog::debug("QQ failed to capture outbound ref_idx: {}", e.what());
        }
    }

    std::string QqChannel::lookup_ref_index(const std::string &ref_idx) const {
        if (ref_idx.empty()) {
            return {};
        }
        std::scoped_lock lock(runtime_->ref_index_mutex);
        const auto it = runtime_->ref_index_cache.find(ref_idx);
        if (it == runtime_->ref_index_cache.end()) {
            return {};
        }
        return it->second;
    }

    void QqChannel::load_ref_index() {
        const auto file_path = qq_ref_index_file_path(bot_name_);
        std::ifstream input(file_path);
        if (!input.is_open()) {
            return;
        }

        try {
            std::scoped_lock lock(runtime_->ref_index_mutex);
            std::string line;
            while (std::getline(input, line)) {
                if (line.empty()) {
                    continue;
                }
                const auto entry = nlohmann::json::parse(line);
                const auto key = entry.value("k", std::string{});
                const auto value = entry.value("v", std::string{});
                if (!key.empty() && !value.empty()) {
                    runtime_->ref_index_cache[key] = value;
                }
            }
            spdlog::debug("QQ loaded {} ref-index entries for bot '{}'", runtime_->ref_index_cache.size(), bot_name_);
        } catch (const std::exception &e) {
            spdlog::warn("Failed to load QQ ref-index: {}", e.what());
        }
    }

    void QqChannel::append_ref_index_line(const std::string &ref_idx, const std::string &content) {
        const auto file_path = qq_ref_index_file_path(bot_name_);
        try {
            std::filesystem::create_directories(file_path.parent_path());
            std::ofstream output(file_path, std::ios::app);
            if (!output.is_open()) {
                return;
            }
            const nlohmann::json line = {{"k", ref_idx}, {"v", content}};
            output << line.dump() << '\n';
        } catch (const std::exception &e) {
            spdlog::debug("Failed to append QQ ref-index line: {}", e.what());
        }
    }

    // ── Typing Indicator ───────────────────────────────────────────────

    void QqChannel::start_typing(const std::string &jid, const std::string &message_id) {
        if (!resolve_typing_target(bot_name_, jid).has_value()) {
            return;
        }
        send_typing_now(jid, message_id);
        {
            std::scoped_lock lock(runtime_->typing_mutex);
            runtime_->typing_states[jid] = RuntimeState::typing_state{
                .message_id = message_id,
                .last_sent_at = std::chrono::steady_clock::now(),
            };
        }
        runtime_->typing_cv.notify_all();
    }

    void QqChannel::stop_typing(const std::string &jid) {
        std::scoped_lock lock(runtime_->typing_mutex);
        runtime_->typing_states.erase(jid);
    }

    void QqChannel::send_typing_now(const std::string &jid, const std::string &message_id) {
        try {
            const auto target = resolve_typing_target(bot_name_, jid);
            if (!target.has_value()) {
                return;
            }
            static_cast<void>(api_client_->post(message_path(*target), build_typing_payload(next_msg_seq(), message_id)));
        } catch (const std::exception &e) {
            spdlog::debug("QQ typing indicator failed for jid '{}': {}", jid, e.what());
        }
    }

    void QqChannel::start_typing_keepalive() {
        stop_typing_keepalive();
        runtime_->typing_thread = std::jthread([this](std::stop_token token) {
            constexpr auto TYPING_INTERVAL = std::chrono::seconds(50);
            while (!token.stop_requested()) {
                std::vector<std::pair<std::string, std::string>> to_send;
                {
                    std::unique_lock lock(runtime_->typing_mutex);
                    runtime_->typing_cv.wait_for(lock, token, std::chrono::seconds(10), [this, &token] {
                        return token.stop_requested() || !runtime_->typing_states.empty();
                    });
                    if (token.stop_requested()) {
                        break;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    for (auto &[jid, state] : runtime_->typing_states) {
                        if (now - state.last_sent_at >= TYPING_INTERVAL) {
                            to_send.emplace_back(jid, state.message_id);
                            state.last_sent_at = now;
                        }
                    }
                }

                for (const auto &[jid, message_id] : to_send) {
                    send_typing_now(jid, message_id);
                }
            }
        });
    }

    void QqChannel::stop_typing_keepalive() {
        if (runtime_->typing_thread.joinable()) {
            runtime_->typing_thread.request_stop();
            runtime_->typing_cv.notify_all();
            runtime_->typing_thread.join();
        }
    }

} // namespace orangutan::channel::qq
