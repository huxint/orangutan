#pragma once

#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace orangutan::channel::qq {

    class QqMessageBuilder {
    public:
        QqMessageBuilder &text(const std::string &content) {
            payload_["content"] = content;
            payload_["msg_type"] = 0;
            return *this;
        }

        QqMessageBuilder &markdown(const std::string &content) {
            payload_["markdown"] = nlohmann::json{
                {"content", content},
            };
            payload_["msg_type"] = 2;
            payload_.erase("content");
            return *this;
        }

        QqMessageBuilder &media(const std::string &file_info) {
            payload_["media"] = nlohmann::json{
                {"file_info", file_info},
            };
            payload_["msg_type"] = 7;
            payload_.erase("content");
            payload_.erase("markdown");
            return *this;
        }

        QqMessageBuilder &msg_seq(base::u16 seq) {
            payload_["msg_seq"] = seq;
            return *this;
        }

        QqMessageBuilder &reply_to(const std::string &msg_id) {
            if (!msg_id.empty()) {
                payload_["msg_id"] = msg_id;
            }
            return *this;
        }

        QqMessageBuilder &reference(const std::string &message_id) {
            if (!message_id.empty()) {
                payload_["message_reference"] = nlohmann::json{
                    {"message_id", message_id},
                    {"ignore_get_message_error", true},
                };
            }
            return *this;
        }

        QqMessageBuilder &keyboard(const nlohmann::json &keyboard_payload) {
            payload_["keyboard"] = keyboard_payload;
            return *this;
        }

        [[nodiscard]]
        nlohmann::json build() const {
            return payload_;
        }

    private:
        nlohmann::json payload_ = nlohmann::json::object();
    };

} // namespace orangutan::channel::qq

namespace orangutan {

    using channel::qq::QqMessageBuilder;

} // namespace orangutan
