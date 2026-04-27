#include "providers/protocols/openai-responses.hpp"

#include <memory>
#include <unordered_map>
#include <utility>

#include "providers/protocols/openai-common.hpp"
#include "providers/protocols/protocol-json.hpp"
#include "utils/json-dump.hpp"

namespace orangutan::providers::protocols {
    namespace {

        [[nodiscard]]
        bool has_reasoning_effort(const ModelTarget &target) noexcept {
            return !target.thinking.empty() && target.thinking != "none";
        }

        [[nodiscard]]
        ProviderError make_openai_responses_protocol_error(std::string_view context, std::string_view detail) {
            return ProviderError(error_category::parsing, "openai responses " + std::string(context) + ": " + std::string(detail));
        }

        [[nodiscard]]
        nlohmann::json parse_openai_responses_payload(std::string_view payload, std::string_view context) {
            return parse_protocol_json_object(payload, "openai responses", context);
        }

        class OpenAiResponsesStreamDecoder final : public StreamDecoder {
        public:
            explicit OpenAiResponsesStreamDecoder(ProviderEventSink sink)
            : sink_(std::move(sink)) {}

            void on_event(std::string_view event_name, std::string_view payload) override {
                try {
                    if (payload == "[DONE]") {
                        return;
                    }

                    const auto event_data = parse_openai_responses_payload(payload, "stream event");

                    if (event_name == "response.output_text.delta") {
                        const auto delta = event_data.value("delta", std::string{});
                        text_ += delta;
                        emit(TextDelta{delta});
                        return;
                    }
                    if (event_name == "response.reasoning_summary_text.delta" || event_name == "response.reasoning.delta") {
                        const auto delta = event_data.value("delta", std::string{});
                        thinking_ += delta;
                        emit(ThinkingDelta{delta});
                        return;
                    }
                    if (event_name == "response.output_item.added") {
                        const auto item = event_data.contains("item") ? event_data["item"] : nlohmann::json::object();
                        if (item.value("type", std::string{}) == "function_call") {
                            auto &call = tool_calls_[item.value("id", item.value("call_id", std::string{}))];
                            call.id = item.value("call_id", item.value("id", std::string{}));
                            call.name = item.value("name", std::string{});
                            if (!call.announced && !call.id.empty() && !call.name.empty()) {
                                emit(ToolCallStarted{.id = call.id, .name = call.name});
                                call.announced = true;
                            }
                        }
                        return;
                    }
                    if (event_name == "response.function_call_arguments.delta") {
                        const auto item_id = event_data.value("item_id", event_data.value("call_id", std::string{}));
                        if (!item_id.empty()) {
                            tool_calls_[item_id].arguments += event_data.value("delta", std::string{});
                        }
                        return;
                    }
                    if (event_name == "response.completed" && event_data.contains("response")) {
                        stop_reason_ = map_stop_reason(event_data["response"].value("status", std::string{"completed"}));
                    }
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_openai_responses_protocol_error("stream event", error.what());
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
                for (const auto &[id, call] : tool_calls_) {
                    static_cast<void>(id);
                    if (auto tool_use = openai::finalize_tool_call(call, "openai responses stream"); tool_use.has_value()) {
                        response.content.emplace_back(std::move(*tool_use));
                    }
                }
                return response;
            }

        private:
            ProviderEventSink sink_;
            std::string text_;
            std::string thinking_;
            std::unordered_map<std::string, openai::ToolCallState> tool_calls_;
            response_stop_reason stop_reason_ = response_stop_reason::end_turn;

            template <typename Event>
            void emit(Event event) {
                if (sink_ != nullptr) {
                    sink_(ProviderEvent{std::move(event)});
                }
            }
        };

        class OpenAiResponsesAdapter final : public ProtocolAdapter {
        public:
            [[nodiscard]]
            transport::HttpRequest build_request(const ModelTarget &target, const ProviderRequest &request) const override {
                nlohmann::json body;
                body["model"] = target.model;
                body["max_output_tokens"] = request.options.max_tokens;
                if (!request.system_prompt.empty()) {
                    body["instructions"] = request.system_prompt;
                }
                if (request.options.stream) {
                    body["stream"] = true;
                }
                if (has_reasoning_effort(target)) {
                    body["reasoning"] = {{"effort", target.thinking}};
                }

                body["input"] = nlohmann::json::array();
                for (const auto &message : request.messages) {
                    openai::append_responses_history_message(body["input"], message);
                }

                if (!request.tools.empty()) {
                    body["tools"] = nlohmann::json::array();
                    for (const auto &tool : request.tools) {
                        body["tools"].push_back(openai::response_tool_to_json(tool));
                    }
                }

                return transport::HttpRequest{
                    .url = target.base_url + "/v1/responses",
                    .body = utils::json_dump_lossy(body),
                    .headers = target.headers,
                };
            }

            [[nodiscard]]
            LLMResponse parse_response(const transport::HttpResponse &response) const override {
                try {
                    const auto payload = parse_openai_responses_payload(response.body, "response");
                    LLMResponse result;
                    result.stop_reason = map_stop_reason(payload.value("status", std::string{"completed"}));

                    if (payload.contains("output_text") && payload["output_text"].is_string() && !payload["output_text"].get<std::string>().empty()) {
                        result.content.emplace_back(Text{payload["output_text"].get<std::string>()});
                    }

                    if (!payload.contains("output") || !payload["output"].is_array()) {
                        return result;
                    }

                    for (const auto &item : payload["output"]) {
                        const auto item_type = item.value("type", std::string{});
                        if (item_type == "message" && item.contains("content")) {
                            for (const auto &content : item["content"]) {
                                const auto content_type = content.value("type", std::string{});
                                if ((content_type == "output_text" || content_type == "text") && content.contains("text")) {
                                    result.content.emplace_back(Text{content["text"].get<std::string>()});
                                } else if ((content_type == "reasoning" || content_type == "reasoning_text") && content.contains("text")) {
                                    result.content.emplace_back(Thinking{content["text"].get<std::string>()});
                                }
                            }
                            continue;
                        }

                        if (item_type == "reasoning" && item.contains("summary") && item["summary"].is_array()) {
                            std::string summary;
                            for (const auto &entry : item["summary"]) {
                                if (entry.contains("text")) {
                                    summary += entry["text"].get<std::string>();
                                }
                            }
                            if (!summary.empty()) {
                                result.content.emplace_back(Thinking{summary});
                            }
                            continue;
                        }

                        if (item_type == "function_call") {
                            openai::ToolCallState state{
                                .id = item.value("call_id", item.value("id", std::string{})),
                                .name = item.value("name", std::string{}),
                                .arguments = item.value("arguments", std::string{}),
                            };
                            if (auto tool_use = openai::finalize_tool_call(state, "openai responses response"); tool_use.has_value()) {
                                result.content.emplace_back(std::move(*tool_use));
                            }
                        }
                    }

                    return result;
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_openai_responses_protocol_error("response", error.what());
                }
            }

            [[nodiscard]]
            std::unique_ptr<StreamDecoder> make_stream_decoder(const ProviderEventSink &sink) const override {
                return std::make_unique<OpenAiResponsesStreamDecoder>(sink);
            }

            [[nodiscard]]
            std::string label() const override {
                return "openai";
            }
        };

    } // namespace

    std::shared_ptr<const ProtocolAdapter> make_openai_responses_adapter() {
        static const auto adapter = std::make_shared<OpenAiResponsesAdapter>();
        return adapter;
    }

} // namespace orangutan::providers::protocols
