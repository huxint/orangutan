#pragma once

#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    class QqMessageBuilder {
    public:
        QqMessageBuilder &text(std::string_view content) {
            payload_["content"] = content;
            payload_["msg_type"] = 0;
            payload_.erase("markdown");
            payload_.erase("media");
            payload_.erase("ark");
            payload_.erase("embed");
            return *this;
        }

        QqMessageBuilder &markdown(std::string_view content) {
            payload_["markdown"] = nlohmann::json{
                {"content", content},
            };
            payload_["msg_type"] = 2;
            payload_.erase("content");
            payload_.erase("media");
            payload_.erase("ark");
            payload_.erase("embed");
            return *this;
        }

        QqMessageBuilder &media(std::string_view file_info, std::string_view content = {}) {
            payload_["media"] = nlohmann::json{
                {"file_info", file_info},
            };
            payload_["msg_type"] = 7;
            if (content.empty()) {
                payload_.erase("content");
            } else {
                payload_["content"] = content;
            }
            payload_.erase("markdown");
            payload_.erase("ark");
            payload_.erase("embed");
            return *this;
        }

        QqMessageBuilder &ark(const nlohmann::json &ark_payload) {
            payload_["ark"] = ark_payload;
            payload_["msg_type"] = 3;
            payload_.erase("content");
            payload_.erase("markdown");
            payload_.erase("media");
            payload_.erase("embed");
            return *this;
        }

        QqMessageBuilder &embed(const nlohmann::json &embed_payload) {
            payload_["embed"] = embed_payload;
            payload_["msg_type"] = 4;
            payload_.erase("content");
            payload_.erase("markdown");
            payload_.erase("media");
            payload_.erase("ark");
            return *this;
        }

        QqMessageBuilder &msg_seq(base::u16 seq) {
            payload_["msg_seq"] = seq;
            return *this;
        }

        QqMessageBuilder &reply_to(std::string_view msg_id) {
            if (!msg_id.empty()) {
                payload_["msg_id"] = msg_id;
            } else {
                payload_.erase("msg_id");
            }
            return *this;
        }

        QqMessageBuilder &reference(std::string_view message_id) {
            if (!message_id.empty()) {
                payload_["message_reference"] = nlohmann::json{
                    {"message_id", message_id},
                    {"ignore_get_message_error", true},
                };
            } else {
                payload_.erase("message_reference");
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
