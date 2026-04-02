#include "channel/qq/qq-channel.hpp"

#include "channel/qq/qq-transport.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ctre.hpp>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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
        constexpr base::u32 intent_public_guild_messages = 1U << 9;
        constexpr base::u32 intent_guild_message_reactions = 1U << 10;
        constexpr base::u32 intent_group_messages = 1U << 25;
        constexpr base::u32 intent_interaction = 1U << 26;
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

        std::filesystem::path qq_known_users_file_path(std::string_view bot_name) {
            const auto home = getenv_or_default("HOME", ".");
            const auto safe_name = sanitize_path_component(std::string(bot_name.empty() ? "default" : bot_name));
            return std::filesystem::path(home) / ".orangutan" / "qq" / "known-users" / ("known-users-" + safe_name + ".json");
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

        template <class... Ts>
        struct Overloaded : Ts... {
            using Ts::operator()...;
        };

        template <class... Ts>
        Overloaded(Ts...) -> Overloaded<Ts...>;

        enum class QqTargetKind {
            c2c,
            group,
            guild,
        };

        struct QqSendTarget {
            QqTargetKind kind;
            std::string id;
        };

        [[nodiscard]]
        QqSendTarget resolve_send_target(std::string_view bot_name, std::string_view jid) {
            const auto c2c_prefix = qq_jid_prefix(bot_name, "c2c");
            const auto group_prefix = qq_jid_prefix(bot_name, "group");
            const auto guild_prefix = qq_jid_prefix(bot_name, "guild");

            if (jid.starts_with(c2c_prefix)) {
                return QqSendTarget{.kind = QqTargetKind::c2c, .id = require_openid(jid, c2c_prefix)};
            }
            if (jid.starts_with(group_prefix)) {
                return QqSendTarget{.kind = QqTargetKind::group, .id = require_openid(jid, group_prefix)};
            }
            if (jid.starts_with(guild_prefix)) {
                return QqSendTarget{.kind = QqTargetKind::guild, .id = require_openid(jid, guild_prefix)};
            }

            throw std::runtime_error("Unsupported QQ jid: " + std::string(jid));
        }

        [[nodiscard]]
        std::string message_path(const QqSendTarget &target) {
            switch (target.kind) {
                case QqTargetKind::c2c:
                    return "/v2/users/" + target.id + "/messages";
                case QqTargetKind::group:
                    return "/v2/groups/" + target.id + "/messages";
                case QqTargetKind::guild:
                    return "/channels/" + target.id + "/messages";
            }

            throw std::runtime_error("Unsupported QQ send target kind");
        }

        [[nodiscard]]
        std::string media_upload_path(const QqSendTarget &target) {
            switch (target.kind) {
                case QqTargetKind::c2c:
                    return "/v2/users/" + target.id + "/files";
                case QqTargetKind::group:
                    return "/v2/groups/" + target.id + "/files";
                case QqTargetKind::guild:
                    throw std::runtime_error("QQ guild currently does not support msg_type=7 media direct send");
            }

            throw std::runtime_error("Unsupported QQ send target kind");
        }

    } // namespace

    struct QqChannel::RuntimeState {
        struct PendingDebouncedMessage {
            std::string text;
            std::string reply_to_message_id;
            std::string reference_message_id;
            std::chrono::steady_clock::time_point first_enqueued_at;
            std::chrono::steady_clock::time_point last_update_at;
        };

        struct KnownUser {
            std::string kind;
            std::string openid;
            std::chrono::system_clock::time_point last_seen_at;
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
        std::mutex known_users_mutex;
        std::unordered_map<std::string, KnownUser> known_users;
        std::mutex group_history_mutex;
        std::unordered_map<std::string, std::deque<std::string>> group_history;
        std::size_t group_history_limit = 50;

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
        load_known_users();
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

    void QqChannel::send(const std::string &jid, const OutboundMessage &message) {
        if (const auto *text = std::get_if<TextPayload>(&message.payload)) {
            enqueue_debounced_text(jid, text->text, message.reply_to_message_id, message.reference_message_id);
            return;
        }

        send_outbound_now(jid, message);
    }

    void QqChannel::send_message_now(const std::string &jid, const std::string &text, const std::string &reply_to_message_id, const std::string &reference_message_id) {
        send_outbound_now(jid, OutboundMessage{
                                   .payload = TextPayload{.text = text},
                                   .reply_to_message_id = reply_to_message_id,
                                   .reference_message_id = reference_message_id,
                               });
    }

    void QqChannel::send_outbound_now(const std::string &jid, const OutboundMessage &message) {
        const auto target = resolve_send_target(bot_name_, jid);
        const auto post_payload = [this, &target](const nlohmann::json &payload) {
            static_cast<void>(api_client_->post(message_path(target), payload));
        };
        const auto send_chunked = [this, &message, &post_payload](const std::string &content, std::size_t limit, auto &&builder_factory) {
            const auto chunks = chunk_text(content, limit);
            const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id, static_cast<int>(chunks.size()));
            const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
            for (const auto &chunk : chunks) {
                post_payload(builder_factory(chunk, passive_reply_message_id, effective_reference));
            }
        };

        std::visit(
            Overloaded{
                [this, &send_chunked](const TextPayload &payload) {
                    send_chunked(payload.text, 4000, [this](const std::string &chunk, const std::string &reply_to_message_id, const std::string &reference_message_id) {
                        return QqMessageBuilder{}.text(chunk).msg_seq(next_msg_seq()).reply_to(reply_to_message_id).reference(reference_message_id).build();
                    });
                },
                [this, &send_chunked](const MarkdownPayload &payload) {
                    send_chunked(payload.markdown, 5000, [this](const std::string &chunk, const std::string &reply_to_message_id, const std::string &reference_message_id) {
                        return QqMessageBuilder{}.markdown(chunk).msg_seq(next_msg_seq()).reply_to(reply_to_message_id).reference(reference_message_id).build();
                    });
                },
                [this, &target, &post_payload, &message](const MediaPayload &payload) {
                    const auto response = api_client_->post(media_upload_path(target), nlohmann::json{
                                                                                           {"file_type", payload.file_type},
                                                                                           {"url", payload.url},
                                                                                           {"srv_send_msg", false},
                                                                                       });
                    const auto upload_payload = response.parse_json_body();
                    if (!upload_payload.contains("file_info") || !upload_payload.at("file_info").is_string()) {
                        throw std::runtime_error("QQ upload media missing file_info");
                    }
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}
                                     .media(upload_payload.at("file_info").get<std::string>(), payload.caption)
                                     .msg_seq(next_msg_seq())
                                     .reply_to(passive_reply_message_id)
                                     .reference(effective_reference)
                                     .build());
                },
                [this, &post_payload, &message](const KeyboardPayload &payload) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}
                                     .markdown(payload.markdown)
                                     .keyboard(payload.keyboard_payload)
                                     .msg_seq(next_msg_seq())
                                     .reply_to(passive_reply_message_id)
                                     .reference(effective_reference)
                                     .build());
                },
                [this, &post_payload, &message](const ArkPayload &payload) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}.ark(payload.ark_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build());
                },
                [this, &post_payload, &message](const EmbedPayload &payload) {
                    const auto passive_reply_message_id = resolve_passive_reply_message_id(message.reply_to_message_id);
                    const auto effective_reference = passive_reply_message_id.empty() ? std::string{} : message.reference_message_id;
                    post_payload(QqMessageBuilder{}.embed(payload.embed_payload).msg_seq(next_msg_seq()).reply_to(passive_reply_message_id).reference(effective_reference).build());
                },
            },
            message.payload);
    }

    Attachment QqChannel::download_attachment(const std::string &jid, const Attachment &attachment, const std::string &destination_path) {
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
        const auto destination = std::filesystem::path(destination_path);
        if (destination.empty()) {
            throw std::runtime_error("attachment destination path must not be empty");
        }
        if (!destination.parent_path().empty()) {
            std::filesystem::create_directories(destination.parent_path());
        }

        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            throw std::runtime_error("unable to open attachment destination for write");
        }
        output.write(response.body.data(), static_cast<std::streamsize>(response.body.size()));
        output.close();
        downloaded.local_path = destination.string();
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
        std::sort(jids.begin(), jids.end());
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
                         {"intents", intent_public_guild_messages | intent_guild_message_reactions | intent_direct_messages | intent_group_messages | intent_interaction |
                                         intent_guild_at_message},
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
                std::vector<std::tuple<std::string, std::string, std::string, std::string>> ready_messages;
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
        runtime_->debounce_stop = true;
        runtime_->debounce_cv.notify_all();
        if (runtime_->debounce_thread.joinable()) {
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
                runtime_->pending_messages[jid] = RuntimeState::PendingDebouncedMessage{
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
            send_message_now(jid, text, reply_to_message_id, reference_message_id);
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

            std::unordered_map<std::string, RuntimeState::KnownUser> loaded;
            for (const auto &item : payload) {
                const auto kind = item.value("kind", std::string{});
                const auto openid = item.value("openid", std::string{});
                if (kind.empty() || openid.empty()) {
                    continue;
                }

                const auto last_seen_text = item.value("last_seen", std::string{});
                const auto last_seen = parse_iso_utc(last_seen_text).value_or(std::chrono::system_clock::now());
                loaded.emplace(kind + ":" + openid, RuntimeState::KnownUser{
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
        std::vector<RuntimeState::KnownUser> users;
        {
            std::scoped_lock lock(runtime_->known_users_mutex);
            users.reserve(runtime_->known_users.size());
            for (const auto &[key, user] : runtime_->known_users) {
                static_cast<void>(key);
                users.push_back(user);
            }
        }

        std::sort(users.begin(), users.end(), [](const auto &lhs, const auto &rhs) {
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
            history = std::move(it->second);
            runtime_->group_history.erase(it);
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
            return;
        }

        if (event_type == "MESSAGE_REACTION_ADD" || event_type == "MESSAGE_REACTION_REMOVE") {
            handle_reaction_event(event_type, data);
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
        remember_known_user("c2c", openid);
        emit_inbound({
            .jid = make_qq_jid(bot_name_, "c2c", openid),
            .sender = sender_id,
            .sender_name = openid,
            .content = data.value("content", std::string{}),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = parse_message_scene_ext_value(data, "ref_msg_idx"),
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
        const auto sender_openid = author.value("member_openid", author.value("user_openid", author.value("id", std::string{})));
        const auto sender_name = author.value("member_openid", author.value("id", std::string{}));
        const auto message_jid = make_qq_jid(bot_name_, "group", group_openid);
        const auto content = strip_mentions(data.value("content", std::string{}));
        const auto mentioned = is_bot_mentioned(data, mention_ids);

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

        emit_inbound({
            .jid = message_jid,
            .sender = author.value("id", std::string{}),
            .sender_name = sender_name,
            .content = aggregated_content,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = parse_message_scene_ext_value(data, "ref_msg_idx"),
            .attachments = parse_attachments(data),
            .mentioned = mentioned,
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
        const auto sender_id = author.value("id", std::string{});
        remember_inbound_message(message_id);
        emit_inbound({
            .jid = make_qq_jid(bot_name_, "guild", channel_id),
            .sender = sender_id,
            .sender_name = author.value("username", sender_id),
            .content = strip_mentions(data.value("content", std::string{})),
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = message_id,
            .reference_message_index = parse_message_scene_ext_value(data, "ref_msg_idx"),
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
        if (!is_guild) {
            remember_known_user("c2c", user_openid);
        }
        emit_inbound({
            .jid = is_guild ? make_qq_jid(bot_name_, "guild", channel_id) : make_qq_jid(bot_name_, "c2c", user_openid),
            .sender = user_openid,
            .sender_name = user_openid,
            .content = button_data,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = interaction_id,
            .is_group = is_guild,
        });
    }

    void QqChannel::handle_reaction_event(const std::string &event_type, const nlohmann::json &data) const {
        if (on_message_ == nullptr) {
            return;
        }

        const auto target = data.value("target", nlohmann::json::object());
        const auto emoji = data.value("emoji", nlohmann::json::object());
        const auto channel_id = data.value("channel_id", std::string{});
        const auto user_id = data.value("user_id", std::string{});
        emit_inbound({
            .event_kind = event_type == "MESSAGE_REACTION_ADD" ? InboundEventKind::reaction_added : InboundEventKind::reaction_removed,
            .jid = make_qq_jid(bot_name_, "guild", channel_id),
            .sender = user_id,
            .sender_name = user_id,
            .timestamp = data.value("timestamp", std::string{}),
            .message_id = target.value("id", data.value("message_id", std::string{})),
            .reaction =
                ReactionInfo{
                    .user_id = user_id,
                    .target_id = target.value("id", std::string{}),
                    .target_type = static_cast<int>(parse_integer_like(target, "type", 0)),
                    .emoji_id = emoji.value("id", std::string{}),
                    .emoji_type = static_cast<int>(parse_integer_like(emoji, "type", 0)),
                },
            .is_group = true,
        });
    }

    void QqChannel::emit_inbound(InboundMessage message) const {
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
        const auto remaining = MessageReplyTracker::max_replies - tracker.reply_count;
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

    std::vector<Attachment> QqChannel::parse_attachments(const nlohmann::json &data) const {
        std::vector<Attachment> attachments;
        if (!data.contains("attachments") || !data.at("attachments").is_array()) {
            return attachments;
        }

        attachments.reserve(data.at("attachments").size());
        for (const auto &item : data.at("attachments")) {
            attachments.push_back(Attachment{
                .content_type = item.value("content_type", std::string{}),
                .url = item.value("url", std::string{}),
                .filename = item.value("filename", std::string{}),
                .width = static_cast<int>(parse_integer_like(item, "width", 0)),
                .height = static_cast<int>(parse_integer_like(item, "height", 0)),
                .size = static_cast<int>(parse_integer_like(item, "size", 0)),
                .download_pending = item.contains("url") && item.at("url").is_string() && !item.at("url").get_ref<const std::string &>().empty(),
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

    std::string QqChannel::parse_message_scene_ext_value(const nlohmann::json &data, std::string_view key) {
        if (!data.contains("message_scene") || !data.at("message_scene").is_object()) {
            return {};
        }

        const auto &scene = data.at("message_scene");
        if (!scene.contains("ext") || !scene.at("ext").is_array()) {
            return {};
        }

        const std::string prefix = std::string(key) + "=";
        for (const auto &item : scene.at("ext")) {
            if (!item.is_string()) {
                continue;
            }
            const auto &value = item.get_ref<const std::string &>();
            if (value.starts_with(prefix)) {
                return value.substr(prefix.size());
            }
        }

        return {};
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
