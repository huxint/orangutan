#pragma once

#include "types/base.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace orangutan::channel::qq {

    class QqMessageBuilder {
    public:
        auto text(this auto &&self, std::string_view content) -> decltype(auto) {
            self.payload_["content"] = content;
            self.payload_["msg_type"] = 0;
            self.payload_.erase("markdown");
            self.payload_.erase("media");
            self.payload_.erase("ark");
            self.payload_.erase("embed");
            return std::forward<decltype(self)>(self);
        }

        auto markdown(this auto &&self, std::string_view content) -> decltype(auto) {
            self.payload_["markdown"] = nlohmann::json{
                {"content", content},
            };
            self.payload_["msg_type"] = 2;
            self.payload_.erase("content");
            self.payload_.erase("media");
            self.payload_.erase("ark");
            self.payload_.erase("embed");
            return std::forward<decltype(self)>(self);
        }

        auto media(this auto &&self, std::string_view file_info, std::string_view content = {}) -> decltype(auto) {
            self.payload_["media"] = nlohmann::json{
                {"file_info", file_info},
            };
            self.payload_["msg_type"] = 7;
            if (content.empty()) {
                self.payload_.erase("content");
            } else {
                self.payload_["content"] = content;
            }
            self.payload_.erase("markdown");
            self.payload_.erase("ark");
            self.payload_.erase("embed");
            return std::forward<decltype(self)>(self);
        }

        auto ark(this auto &&self, const nlohmann::json &ark_payload) -> decltype(auto) {
            self.payload_["ark"] = ark_payload;
            self.payload_["msg_type"] = 3;
            self.payload_.erase("content");
            self.payload_.erase("markdown");
            self.payload_.erase("media");
            self.payload_.erase("embed");
            return std::forward<decltype(self)>(self);
        }

        auto embed(this auto &&self, const nlohmann::json &embed_payload) -> decltype(auto) {
            self.payload_["embed"] = embed_payload;
            self.payload_["msg_type"] = 4;
            self.payload_.erase("content");
            self.payload_.erase("markdown");
            self.payload_.erase("media");
            self.payload_.erase("ark");
            return std::forward<decltype(self)>(self);
        }

        auto msg_seq(this auto &&self, std::uint16_t seq) -> decltype(auto) {
            self.payload_["msg_seq"] = seq;
            return std::forward<decltype(self)>(self);
        }

        auto reply_to(this auto &&self, std::string_view msg_id) -> decltype(auto) {
            if (!msg_id.empty()) {
                self.payload_["msg_id"] = msg_id;
            } else {
                self.payload_.erase("msg_id");
            }
            return std::forward<decltype(self)>(self);
        }

        auto reference(this auto &&self, std::string_view message_id) -> decltype(auto) {
            if (!message_id.empty()) {
                self.payload_["message_reference"] = nlohmann::json{
                    {"message_id", message_id},
                    {"ignore_get_message_error", true},
                };
            } else {
                self.payload_.erase("message_reference");
            }
            return std::forward<decltype(self)>(self);
        }

        auto keyboard(this auto &&self, const nlohmann::json &keyboard_payload) -> decltype(auto) {
            self.payload_["keyboard"] = keyboard_payload;
            return std::forward<decltype(self)>(self);
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
