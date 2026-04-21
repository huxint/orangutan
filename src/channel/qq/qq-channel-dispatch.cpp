#include "channel/qq/qq-channel.hpp"

#include "channel/qq/qq-api-client.hpp"
#include "channel/qq/qq-channel-inbound.hpp"
#include "channel/qq/qq-channel-outbound.hpp"
#include "channel/qq/qq-channel-runtime.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    void QqChannel::handle_ws_message(const std::string &data) {
        const auto payload = nlohmann::json::parse(data);

        if (payload.contains("s") && !payload.at("s").is_null()) {
            {
                std::scoped_lock lock(runtime_->mutex);
                runtime_->last_seq = payload.at("s").get<std::uint32_t>();
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
                    spdlog::info("qq gateway ready for bot '{}'", event_data.value("session_id", std::string{"unknown"}));
                } else if (event_type == "RESUMED") {
                    {
                        std::scoped_lock lock(runtime_->mutex);
                        runtime_->ready = true;
                        runtime_->last_error.clear();
                        connected_ = true;
                        runtime_->cv.notify_all();
                    }
                    should_persist = true;
                    spdlog::info("qq gateway session resumed");
                }

                if (should_persist) {
                    persist_session_state();
                }

                handle_dispatch(event_type, event_data);
                break;
            }
            case GATEWAY_OP_HEARTBEAT_ACK:
                spdlog::debug("qq heartbeat acknowledged");
                break;
            case GATEWAY_OP_RECONNECT:
                spdlog::warn("qq gateway requested reconnect");
                connected_ = false;
                stop_heartbeat();
#ifdef ORANGUTAN_ENABLE_QQ_CHANNEL
                if (runtime_->websocket != nullptr) {
                    runtime_->websocket->request_reconnect();
                }
#endif
                break;
            case GATEWAY_OP_INVALID_SESSION:
                spdlog::warn("qq gateway reported invalid session");
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
                spdlog::debug("unhandled qq gateway opcode: {}", op);
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
                spdlog::warn("qq interaction ack failed for '{}': {}", interaction_id, e.what());
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

} // namespace orangutan::channel::qq
