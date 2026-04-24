#include "providers/protocols/openai-chat-completions.hpp"

#include <memory>
#include <utility>

#include "providers/protocols/openai-common.hpp"
#include "utils/json-dump.hpp"

namespace orangutan::providers::protocols {
    namespace {

        [[nodiscard]]
        bool has_reasoning_effort(const ModelTarget &target) noexcept {
            return !target.thinking.empty() && target.thinking != "none";
        }

        [[nodiscard]]
        ProviderError make_openai_chat_protocol_error(std::string_view context, std::string_view detail) {
            return ProviderError(error_category::parsing, "openai chat completions " + std::string(context) + ": " + std::string(detail));
        }

        [[nodiscard]]
        nlohmann::json parse_openai_chat_payload(std::string_view payload, std::string_view context) {
            try {
                const auto event_data = nlohmann::json::parse(payload);
                if (!event_data.is_object()) {
                    throw make_openai_chat_protocol_error(context, "expected a json object");
                }
                return event_data;
            } catch (const ProviderError &) {
                throw;
            } catch (const nlohmann::json::exception &error) {
                throw make_openai_chat_protocol_error(context, error.what());
            }
        }

        class OpenAiChatStreamDecoder final : public StreamDecoder {
        public:
            explicit OpenAiChatStreamDecoder(ProviderEventSink sink)
            : sink_(std::move(sink)) {}

            void on_event(std::string_view /*event_name*/, std::string_view payload) override {
                try {
                    if (payload == "[DONE]") {
                        return;
                    }

                    const auto event_data = parse_openai_chat_payload(payload, "stream event");
                    if (!event_data.contains("choices") || !event_data["choices"].is_array() || event_data["choices"].empty()) {
                        return;
                    }

                    const auto &choice = event_data["choices"][0];
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        stop_reason_ = map_stop_reason(choice["finish_reason"].get<std::string>());
                    }

                    if (!choice.contains("delta")) {
                        return;
                    }

                    const auto &delta = choice["delta"];
                    if (delta.contains("content") && !delta["content"].is_null()) {
                        const auto text = delta["content"].get<std::string>();
                        text_ += text;
                        emit(TextDelta{text});
                    }
                    if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
                        const auto thinking = delta["reasoning_content"].get<std::string>();
                        thinking_ += thinking;
                        emit(ThinkingDelta{thinking});
                    }
                    if (delta.contains("tool_calls")) {
                        for (const auto &tool_call : delta["tool_calls"]) {
                            if (!tool_call.contains("index") || !tool_call["index"].is_number_integer()) {
                                continue;
                            }
                            const auto index = tool_call["index"].get<std::size_t>();
                            while (tool_calls_.size() <= index) {
                                tool_calls_.push_back({});
                            }

                            auto &state = tool_calls_[index];
                            openai::merge_tool_call_delta(state, tool_call);
                            if (!state.announced && !state.id.empty() && !state.name.empty()) {
                                emit(ToolCallStarted{.id = state.id, .name = state.name});
                                state.announced = true;
                            }
                        }
                    }
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_openai_chat_protocol_error("stream event", error.what());
                }
            }

            [[nodiscard]]
            LLMResponse finish() const override {
                LLMResponse response;
                response.stop_reason = stop_reason_;
                if (!thinking_.empty()) {
                    response.content.emplace_back(Thinking{thinking_});
                }
                if (!text_.empty()) {
                    response.content.emplace_back(Text{text_});
                }
                for (const auto &state : tool_calls_) {
                    if (auto tool_use = openai::finalize_tool_call(state, "openai chat completions stream"); tool_use.has_value()) {
                        response.content.emplace_back(std::move(*tool_use));
                    }
                }
                return response;
            }

        private:
            ProviderEventSink sink_;
            std::string text_;
            std::string thinking_;
            std::vector<openai::ToolCallState> tool_calls_;
            response_stop_reason stop_reason_ = response_stop_reason::end_turn;

            template <typename Event>
            void emit(Event event) {
                if (sink_ != nullptr) {
                    sink_(ProviderEvent{std::move(event)});
                }
            }
        };

        class OpenAiChatCompletionsAdapter final : public ProtocolAdapter {
        public:
            [[nodiscard]]
            transport::HttpRequest build_request(const ModelTarget &target, const ProviderRequest &request) const override {
                nlohmann::json body;
                body["model"] = target.model;
                body["max_tokens"] = request.options.max_tokens;
                if (request.options.stream) {
                    body["stream"] = true;
                }
                if (has_reasoning_effort(target)) {
                    body["reasoning_effort"] = target.thinking;
                }

                body["messages"] = nlohmann::json::array();
                if (!request.system_prompt.empty()) {
                    body["messages"].push_back({{"role", "system"}, {"content", request.system_prompt}});
                }
                for (const auto &message : request.messages) {
                    openai::append_chat_history_message(body["messages"], message);
                }

                if (!request.tools.empty()) {
                    body["tools"] = nlohmann::json::array();
                    for (const auto &tool : request.tools) {
                        body["tools"].push_back(openai::chat_tool_to_json(tool));
                    }
                }

                return transport::HttpRequest{
                    .url = target.base_url + "/v1/chat/completions",
                    .body = utils::json_dump_lossy(body),
                    .headers = target.headers,
                };
            }

            [[nodiscard]]
            LLMResponse parse_response(const transport::HttpResponse &response) const override {
                try {
                    const auto payload = parse_openai_chat_payload(response.body, "response");
                    LLMResponse result;
                    if (!payload.contains("choices") || !payload["choices"].is_array() || payload["choices"].empty()) {
                        result.stop_reason = response_stop_reason::unknown;
                        return result;
                    }

                    const auto &choice = payload["choices"][0];
                    result.stop_reason = map_stop_reason(choice.value("finish_reason", "stop"));
                    const auto &message = choice["message"];

                    if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
                        result.content.emplace_back(Thinking{message["reasoning_content"].get<std::string>()});
                    }
                    if (message.contains("content") && !message["content"].is_null()) {
                        result.content.emplace_back(Text{message["content"].get<std::string>()});
                    }
                    if (message.contains("tool_calls")) {
                        for (const auto &tool_call : message["tool_calls"]) {
                            openai::ToolCallState state;
                            openai::merge_tool_call_delta(state, tool_call);
                            if (auto tool_use = openai::finalize_tool_call(state, "openai chat completions response"); tool_use.has_value()) {
                                result.content.emplace_back(std::move(*tool_use));
                            }
                        }
                    }

                    return result;
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_openai_chat_protocol_error("response", error.what());
                }
            }

            [[nodiscard]]
            std::unique_ptr<StreamDecoder> make_stream_decoder(const ProviderEventSink &sink) const override {
                return std::make_unique<OpenAiChatStreamDecoder>(sink);
            }

            [[nodiscard]]
            std::string label() const override {
                return "openai";
            }
        };

    } // namespace

    std::shared_ptr<const ProtocolAdapter> make_openai_chat_completions_adapter() {
        static const auto adapter = std::make_shared<OpenAiChatCompletionsAdapter>();
        return adapter;
    }

} // namespace orangutan::providers::protocols
