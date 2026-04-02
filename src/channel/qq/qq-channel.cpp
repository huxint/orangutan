#include "channel/qq/qq-channel.hpp"

#include "channel/qq/qq-transport.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctre.hpp>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
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

        std::string sanitize_path_component(std::string input) {
            for (auto &ch : input) {
                if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '-' && ch != '_') {
                    ch = '_';
                }
            }
            if (input.empty()) {
                return "default";
            }
            return input;
        }

        std::filesystem::path qq_session_file_path(std::string_view bot_name) {
            const auto home = getenv_or_default("HOME", ".");
            const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
            return std::filesystem::path(home) / ".orangutan" / "qq" / "sessions" / ("session-" + safe_name + ".json");
        }

        std::optional<int> parse_fixed_int(std::string_view value, std::size_t offset, std::size_t width) {
            if (offset + width > value.size()) {
                return std::nullopt;
            }

            int result = 0;
            const auto token = value.substr(offset, width);
            auto [ptr, ec] = std::from_chars(token.begin(), token.end(), result);
            if (ec != std::errc{} || ptr != token.end()) {
                return std::nullopt;
            }
            return result;
        }

        std::string format_iso_utc(std::chrono::system_clock::time_point tp) {
            const auto truncated = std::chrono::floor<std::chrono::seconds>(tp);
            const auto day = std::chrono::floor<std::chrono::days>(truncated);
            const auto ymd = std::chrono::year_month_day{day};
            if (!ymd.ok()) {
                return {};
            }

            const auto tod = std::chrono::hh_mm_ss{truncated - day};
            return spdlog::fmt_lib::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                           static_cast<unsigned>(ymd.day()), static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()),
                                           static_cast<int>(tod.seconds().count()));
        }

        std::optional<std::chrono::system_clock::time_point> parse_iso_utc(std::string_view iso) {
            if (iso.size() != 20 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':' || iso[19] != 'Z') {
                return std::nullopt;
            }

            const auto year = parse_fixed_int(iso, 0, 4);
            const auto month = parse_fixed_int(iso, 5, 2);
            const auto day = parse_fixed_int(iso, 8, 2);
            const auto hour = parse_fixed_int(iso, 11, 2);
            const auto minute = parse_fixed_int(iso, 14, 2);
            const auto second = parse_fixed_int(iso, 17, 2);
            if (!year.has_value() || !month.has_value() || !day.has_value() || !hour.has_value() || !minute.has_value() || !second.has_value()) {
                return std::nullopt;
            }
            if (*hour > 23 || *minute > 59 || *second > 59) {
                return std::nullopt;
            }

            const auto ymd = std::chrono::year{*year} / std::chrono::month{static_cast<unsigned>(*month)} / std::chrono::day{static_cast<unsigned>(*day)};
            if (!ymd.ok()) {
                return std::nullopt;
            }

            const auto utc_time = std::chrono::sys_days{ymd} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} + std::chrono::seconds{*second};
            return std::chrono::time_point_cast<std::chrono::system_clock::duration>(utc_time);
        }

    } // namespace

    struct QqChannel::RuntimeState {
        struct PendingDebouncedMessage {
            std::string text;
            std::string reply_to_message_id;
            std::chrono::steady_clock::time_point first_enqueued_at;
            std::chrono::steady_clock::time_point last_update_at;
        };

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
        std::mutex token_refresh_mutex;
        std::condition_variable token_refresh_cv;
        std::atomic<bool> token_refresh_stop{false};
        std::thread token_refresh_thread;
        std::mutex debounce_mutex;
        std::condition_variable debounce_cv;
        std::atomic<bool> debounce_stop{false};
        std::thread debounce_thread;
        std::unordered_map<std::string, PendingDebouncedMessage> pending_messages;
        std::chrono::milliseconds debounce_window{1500};
        std::chrono::milliseconds debounce_max_wait{8000};
        std::string debounce_separator = "\n\n---\n\n";

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
        load_session_state();
        start_token_refresh_loop();
        start_debounce_loop();

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
        enqueue_debounced_text(jid, text, reply_to_message_id);
    }

    void QqChannel::send_message_now(const std::string &jid, const std::string &text, const std::string &reply_to_message_id) {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");

        if (jid.starts_with(c2c_prefix)) {
            send_c2c(require_openid(jid, c2c_prefix), text, reply_to_message_id, reply_to_message_id);
            return;
        }

        if (jid.starts_with(group_prefix)) {
            send_group(require_openid(jid, group_prefix), text, reply_to_message_id, reply_to_message_id);
            return;
        }

        if (jid.starts_with(guild_prefix)) {
            send_guild(require_openid(jid, guild_prefix), text, reply_to_message_id, reply_to_message_id);
            return;
        }

        throw std::runtime_error("Unsupported QQ jid: " + jid);
    }

    void QqChannel::send_markdown_message(const std::string &jid, const std::string &markdown, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");

        if (jid.starts_with(c2c_prefix)) {
            send_markdown_c2c(require_openid(jid, c2c_prefix), markdown, reply_to_message_id, reference_message_id);
            return;
        }

        if (jid.starts_with(group_prefix)) {
            send_markdown_group(require_openid(jid, group_prefix), markdown, reply_to_message_id, reference_message_id);
            return;
        }

        if (jid.starts_with(guild_prefix)) {
            send_markdown_guild(require_openid(jid, guild_prefix), markdown, reply_to_message_id, reference_message_id);
            return;
        }

        throw std::runtime_error("Unsupported QQ jid: " + jid);
    }

    void QqChannel::send_media_message(const std::string &jid, int file_type, const std::string &url, const std::string &reply_to_message_id) {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");

        if (jid.starts_with(c2c_prefix)) {
            const auto openid = require_openid(jid, c2c_prefix);
            const auto file_info = upload_media_c2c(openid, file_type, url);
            send_media_c2c(openid, file_info, reply_to_message_id);
            return;
        }

        if (jid.starts_with(group_prefix)) {
            const auto openid = require_openid(jid, group_prefix);
            const auto file_info = upload_media_group(openid, file_type, url);
            send_media_group(openid, file_info, reply_to_message_id);
            return;
        }

        if (jid.starts_with(guild_prefix)) {
            throw std::runtime_error("QQ guild currently does not support msg_type=7 media direct send");
        }

        throw std::runtime_error("Unsupported QQ jid: " + jid);
    }

    void QqChannel::send_keyboard_message(const std::string &jid, const std::string &markdown, const nlohmann::json &keyboard_payload, const std::string &reply_to_message_id) {
        const auto c2c_prefix = qq_jid_prefix(bot_name_, "c2c");
        const auto group_prefix = qq_jid_prefix(bot_name_, "group");
        const auto guild_prefix = qq_jid_prefix(bot_name_, "guild");
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);

        auto payload = QqMessageBuilder{}.markdown(markdown).keyboard(keyboard_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).build();

        if (jid.starts_with(c2c_prefix)) {
            static_cast<void>(api_client_->post("/v2/users/" + require_openid(jid, c2c_prefix) + "/messages", payload));
            return;
        }
        if (jid.starts_with(group_prefix)) {
            static_cast<void>(api_client_->post("/v2/groups/" + require_openid(jid, group_prefix) + "/messages", payload));
            return;
        }
        if (jid.starts_with(guild_prefix)) {
            static_cast<void>(api_client_->post("/channels/" + require_openid(jid, guild_prefix) + "/messages", payload));
            return;
        }

        throw std::runtime_error("Unsupported QQ jid: " + jid);
    }

    void QqChannel::add_reaction(const std::string &channel_id, const std::string &message_id, const std::string &type, const std::string &id) {
        static_cast<void>(api_client_->put("/channels/" + channel_id + "/messages/" + message_id + "/reactions/" + type + "/" + id, nlohmann::json::object()));
    }

    void QqChannel::remove_reaction(const std::string &channel_id, const std::string &message_id, const std::string &type, const std::string &id) {
        static_cast<void>(api_client_->del("/channels/" + channel_id + "/messages/" + message_id + "/reactions/" + type + "/" + id));
    }

    void QqChannel::disconnect() {
        connected_ = false;
        stop_heartbeat();
        stop_token_refresh_loop();
        stop_debounce_loop();

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

    void QqChannel::start_token_refresh_loop() {
        stop_token_refresh_loop();

        runtime_->token_refresh_stop = false;
        runtime_->token_refresh_thread = std::thread([this] {
            while (!runtime_->token_refresh_stop.load()) {
                std::unique_lock lock(runtime_->token_refresh_mutex);
                const bool should_stop = runtime_->token_refresh_cv.wait_for(lock, std::chrono::minutes(1), [this] {
                    return runtime_->token_refresh_stop.load();
                });
                lock.unlock();

                if (should_stop || runtime_->token_refresh_stop.load()) {
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
        runtime_->token_refresh_stop = true;
        runtime_->token_refresh_cv.notify_all();
        if (runtime_->token_refresh_thread.joinable()) {
            runtime_->token_refresh_thread.join();
        }
    }

    void QqChannel::start_debounce_loop() {
        stop_debounce_loop();

        runtime_->debounce_stop = false;
        runtime_->debounce_thread = std::thread([this] {
            while (!runtime_->debounce_stop.load()) {
                std::vector<std::tuple<std::string, std::string, std::string>> ready_messages;
                {
                    std::unique_lock lock(runtime_->debounce_mutex);
                    runtime_->debounce_cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
                        return runtime_->debounce_stop.load() || !runtime_->pending_messages.empty();
                    });
                    if (runtime_->debounce_stop.load()) {
                        break;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    for (auto it = runtime_->pending_messages.begin(); it != runtime_->pending_messages.end();) {
                        const auto elapsed_since_update = now - it->second.last_update_at;
                        const auto elapsed_since_first = now - it->second.first_enqueued_at;
                        if (elapsed_since_update >= runtime_->debounce_window || elapsed_since_first >= runtime_->debounce_max_wait) {
                            ready_messages.emplace_back(it->first, it->second.text, it->second.reply_to_message_id);
                            it = runtime_->pending_messages.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }

                for (const auto &[jid, text, reply_to] : ready_messages) {
                    flush_debounced_text(jid, text, reply_to);
                }
            }
        });
    }

    void QqChannel::stop_debounce_loop() {
        runtime_->debounce_stop = true;
        runtime_->debounce_cv.notify_all();
        if (runtime_->debounce_thread.joinable()) {
            runtime_->debounce_thread.join();
        }

        std::vector<std::tuple<std::string, std::string, std::string>> remaining_messages;
        {
            std::scoped_lock lock(runtime_->debounce_mutex);
            for (const auto &[jid, pending] : runtime_->pending_messages) {
                remaining_messages.emplace_back(jid, pending.text, pending.reply_to_message_id);
            }
            runtime_->pending_messages.clear();
        }
        if (!connected_.load()) {
            return;
        }
        for (const auto &[jid, text, reply_to] : remaining_messages) {
            flush_debounced_text(jid, text, reply_to);
        }
    }

    void QqChannel::enqueue_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id) {
        if (text.empty()) {
            return;
        }

        std::optional<std::tuple<std::string, std::string, std::string>> immediate_flush;
        {
            std::scoped_lock lock(runtime_->debounce_mutex);
            const auto now = std::chrono::steady_clock::now();
            auto it = runtime_->pending_messages.find(jid);
            if (it != runtime_->pending_messages.end() && it->second.reply_to_message_id != reply_to_message_id) {
                immediate_flush.emplace(jid, it->second.text, it->second.reply_to_message_id);
                runtime_->pending_messages.erase(it);
                it = runtime_->pending_messages.end();
            }

            if (it == runtime_->pending_messages.end()) {
                runtime_->pending_messages[jid] = RuntimeState::PendingDebouncedMessage{
                    .text = text,
                    .reply_to_message_id = reply_to_message_id,
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
            const auto &[flush_jid, flush_text, flush_reply_to] = *immediate_flush;
            flush_debounced_text(flush_jid, flush_text, flush_reply_to);
        }
        runtime_->debounce_cv.notify_all();
    }

    void QqChannel::flush_debounced_text(const std::string &jid, const std::string &text, const std::string &reply_to_message_id) {
        try {
            send_message_now(jid, text, reply_to_message_id);
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
            handle_c2c_message(data);
            return;
        }

        if (event_type == "GROUP_AT_MESSAGE_CREATE" || event_type == "GROUP_MESSAGE_CREATE") {
            handle_group_message(data);
            return;
        }

        if (event_type == "AT_MESSAGE_CREATE" || event_type == "GUILD_MESSAGE_CREATE") {
            handle_guild_message(data);
            return;
        }

        if (event_type == "INTERACTION_CREATE") {
            handle_interaction(data);
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

    void QqChannel::handle_guild_message(const nlohmann::json &data) {
        if (on_message_ == nullptr) {
            return;
        }

        const auto author = data.value("author", nlohmann::json::object());
        const auto channel_id = data.value("channel_id", std::string{});
        const auto message_id = data.value("id", std::string{});
        const auto mention_ids = parse_mention_ids(data);
        remember_inbound_message(message_id);
        on_message_({
            .jid = make_qq_jid(bot_name_, "guild", channel_id),
            .sender = author.value("id", std::string{}),
            .sender_name = author.value("username", author.value("id", std::string{})),
            .content = strip_mentions(data.value("content", std::string{})),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .attachments = parse_attachments(data),
            .mentioned = is_bot_mentioned(data, mention_ids),
            .mention_ids = mention_ids,
            .is_group = true,
        });
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

        std::string button_data;
        if (data.contains("data")) {
            const auto event_data = data.value("data", nlohmann::json::object());
            const auto resolved = event_data.value("resolved", nlohmann::json::object());
            button_data = resolved.value("button_data", std::string{});
            if (button_data.empty()) {
                button_data = event_data.value("button_data", std::string{});
            }
        }

        const auto user_openid = data.value("user_openid", std::string{});
        const auto channel_id = data.value("channel_id", std::string{});
        const bool is_guild = !channel_id.empty();
        on_message_({
            .jid = is_guild ? make_qq_jid(bot_name_, "guild", channel_id) : make_qq_jid(bot_name_, "c2c", user_openid),
            .sender = user_openid,
            .sender_name = user_openid,
            .content = button_data,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = interaction_id,
            .is_group = is_guild,
        });
    }

    void QqChannel::send_c2c(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 4000)) {
            auto payload = QqMessageBuilder{}.text(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/v2/users/" + openid + "/messages", payload));
        }
    }

    void QqChannel::send_group(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 4000)) {
            auto payload = QqMessageBuilder{}.text(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/v2/groups/" + openid + "/messages", payload));
        }
    }

    void QqChannel::send_guild(const std::string &channel_id, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 4000)) {
            auto payload = QqMessageBuilder{}.text(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/channels/" + channel_id + "/messages", payload));
        }
    }

    void QqChannel::send_markdown_c2c(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 5000)) {
            auto payload = QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/v2/users/" + openid + "/messages", payload));
        }
    }

    void QqChannel::send_markdown_group(const std::string &openid, const std::string &content, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 5000)) {
            auto payload = QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/v2/groups/" + openid + "/messages", payload));
        }
    }

    void QqChannel::send_markdown_guild(const std::string &channel_id, const std::string &content, const std::string &reply_to_message_id,
                                        const std::string &reference_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : reference_message_id;
        for (const auto &chunk : chunk_text(content, 5000)) {
            auto payload = QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build();
            static_cast<void>(api_client_->post("/channels/" + channel_id + "/messages", payload));
        }
    }

    std::string QqChannel::upload_media_c2c(const std::string &openid, int file_type, const std::string &url) {
        const auto response = api_client_->post("/v2/users/" + openid + "/files", nlohmann::json{
                                                                                      {"file_type", file_type},
                                                                                      {"url", url},
                                                                                      {"srv_send_msg", false},
                                                                                  });
        const auto payload = response.parse_json_body();
        if (!payload.contains("file_info") || !payload.at("file_info").is_string()) {
            throw std::runtime_error("QQ upload media (c2c) missing file_info");
        }
        return payload.at("file_info").get<std::string>();
    }

    std::string QqChannel::upload_media_group(const std::string &openid, int file_type, const std::string &url) {
        const auto response = api_client_->post("/v2/groups/" + openid + "/files", nlohmann::json{
                                                                                       {"file_type", file_type},
                                                                                       {"url", url},
                                                                                       {"srv_send_msg", false},
                                                                                   });
        const auto payload = response.parse_json_body();
        if (!payload.contains("file_info") || !payload.at("file_info").is_string()) {
            throw std::runtime_error("QQ upload media (group) missing file_info");
        }
        return payload.at("file_info").get<std::string>();
    }

    void QqChannel::send_media_c2c(const std::string &openid, const std::string &file_info, const std::string &reply_to_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        auto payload = QqMessageBuilder{}.media(file_info).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).build();
        static_cast<void>(api_client_->post("/v2/users/" + openid + "/messages", payload));
    }

    void QqChannel::send_media_group(const std::string &openid, const std::string &file_info, const std::string &reply_to_message_id) {
        const auto passive_reply_message_id = resolve_passive_reply_message_id(reply_to_message_id);
        auto payload = QqMessageBuilder{}.media(file_info).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).build();
        static_cast<void>(api_client_->post("/v2/groups/" + openid + "/messages", payload));
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

    std::string QqChannel::resolve_passive_reply_message_id(const std::string &reply_to_message_id) {
        if (reply_to_message_id.empty()) {
            return {};
        }
        if (consume_passive_reply_quota(reply_to_message_id)) {
            return reply_to_message_id;
        }
        spdlog::warn("QQ passive reply quota exceeded or expired for message_id='{}', falling back to proactive send", reply_to_message_id);
        return {};
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
        std::string stripped;
        stripped.reserve(content.size());
        const auto content_view = std::string_view{content};
        const char *cursor = content_view.data();
        const char *const end = cursor + content_view.size();

        for (const auto match : ctre::search_all<R"(<@!?[^>]+>)">(content_view)) {
            const auto full = match.get<0>().to_view();
            stripped.append(cursor, full.data() - cursor);
            cursor = full.data() + full.size();
        }
        stripped.append(cursor, static_cast<std::size_t>(end - cursor));
        return std::string(trim_copy(stripped));
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
