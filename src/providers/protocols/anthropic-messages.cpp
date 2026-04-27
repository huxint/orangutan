#include "providers/protocols/anthropic-messages.hpp"

#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "providers/protocols/protocol-json.hpp"
#include "types/serialization.hpp"
#include "utils/json-dump.hpp"

namespace orangutan::providers::protocols {
    namespace {

        [[nodiscard]]
        int anthropic_thinking_budget(std::string_view thinking) noexcept {
            if (thinking == "low") {
                return 1024;
            }
            if (thinking == "medium") {
                return 4096;
            }
            if (thinking == "high") {
                return 8192;
            }
            if (thinking == "xhigh") {
                return 16384;
            }
            return 0;
        }

        [[nodiscard]]
        ProviderError make_anthropic_messages_protocol_error(std::string_view context, std::string_view detail) {
            return ProviderError(error_category::parsing, "anthropic messages " + std::string(context) + ": " + std::string(detail));
        }

        [[nodiscard]]
        nlohmann::json parse_anthropic_messages_payload(std::string_view payload, std::string_view context) {
            return parse_protocol_json_object(payload, "anthropic messages", context);
        }

        class AnthropicMessagesStreamDecoder final : public StreamDecoder {
        public:
            explicit AnthropicMessagesStreamDecoder(ProviderEventSink sink)
            : sink_(std::move(sink)) {}

            void on_event(std::string_view /*event_name*/, std::string_view payload) override {
                try {
                    if (payload == "[DONE]") {
                        return;
                    }

                    const auto event_data = parse_anthropic_messages_payload(payload, "stream event");

                    const auto type = event_data.value("type", std::string{});
                    if (type == "content_block_start") {
                        auto block = event_data["content_block"];
                        BlockState state;
                        state.type = block.value("type", std::string{});
                        if (state.type == "tool_use") {
                            state.id = block.value("id", std::string{});
                            state.name = block.value("name", std::string{});
                            emit(ToolCallStarted{.id = state.id, .name = state.name});
                        }
                        blocks_.push_back(std::move(state));
                        return;
                    }

                    if (type == "content_block_delta") {
                        const auto index = event_data.value("index", std::size_t{0});
                        if (index >= blocks_.size()) {
                            return;
                        }

                        auto &block = blocks_[index];
                        const auto &delta = event_data["delta"];
                        const auto delta_type = delta.value("type", std::string{});
                        if (delta_type == "text_delta") {
                            const auto text = delta["text"].get<std::string>();
                            block.text += text;
                            emit(TextDelta{text});
                        } else if (delta_type == "input_json_delta") {
                            block.input_json += delta["partial_json"].get<std::string>();
                        } else if (delta_type == "thinking_delta") {
                            const auto thinking = delta["thinking"].get<std::string>();
                            block.text += thinking;
                            emit(ThinkingDelta{thinking});
                        }
                        return;
                    }

                    if (type == "message_delta" && event_data.contains("delta") && event_data["delta"].contains("stop_reason")) {
                        stop_reason_ = map_stop_reason(event_data["delta"]["stop_reason"].get<std::string>());
                    }
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_anthropic_messages_protocol_error("stream event", error.what());
                }
            }

            [[nodiscard]]
            LLMResponse finish() const override {
                try {
                    LLMResponse response;
                    response.stop_reason = stop_reason_;

                    for (const auto &block : blocks_) {
                        if (block.type == "text") {
                            response.content.emplace_back(Text{block.text});
                        } else if (block.type == "thinking") {
                            response.content.emplace_back(Thinking{block.text});
                        } else if (block.type == "tool_use") {
                            nlohmann::json input = nlohmann::json::object();
                            if (!block.input_json.empty()) {
                                const auto parsed = nlohmann::json::parse(block.input_json);
                                if (!parsed.is_object()) {
                                    throw make_anthropic_messages_protocol_error("tool input", "expected a json object");
                                }
                                input = parsed;
                            }
                            response.content.emplace_back(ToolUse(block.id, block.name, std::move(input)));
                        }
                    }

                    return response;
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_anthropic_messages_protocol_error("stream result", error.what());
                }
            }

        private:
            struct BlockState {
                std::string type;
                std::string text;
                std::string id;
                std::string name;
                std::string input_json;
            };

            ProviderEventSink sink_;
            std::vector<BlockState> blocks_;
            response_stop_reason stop_reason_ = response_stop_reason::end_turn;

            template <typename Event>
            void emit(Event event) {
                if (sink_ != nullptr) {
                    sink_(ProviderEvent{std::move(event)});
                }
            }
        };

        class AnthropicMessagesAdapter final : public ProtocolAdapter {
        public:
            [[nodiscard]]
            transport::HttpRequest build_request(const ModelTarget &target, const ProviderRequest &request) const override {
                nlohmann::json body;
                body["model"] = target.model;
                body["max_tokens"] = request.options.max_tokens;
                body["system"] = request.system_prompt;
                if (request.options.stream) {
                    body["stream"] = true;
                }

                body["messages"] = nlohmann::json::array();
                for (const auto &message : request.messages) {
                    body["messages"].push_back(message_to_json(message));
                }

                if (!request.tools.empty()) {
                    body["tools"] = nlohmann::json::array();
                    for (const auto &tool : request.tools) {
                        body["tools"].push_back({{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.input_schema}});
                    }
                }

                const auto resolved_budget = request.options.thinking_budget > 0 ? request.options.thinking_budget : anthropic_thinking_budget(target.thinking);
                if (resolved_budget > 0) {
                    body["thinking"] = {{"type", "enabled"}, {"budget_tokens", resolved_budget}};
                }

                return transport::HttpRequest{
                    .url = target.base_url + "/v1/messages",
                    .body = utils::json_dump_lossy(body),
                    .headers = target.headers,
                };
            }

            [[nodiscard]]
            LLMResponse parse_response(const transport::HttpResponse &response) const override {
                try {
                    const auto payload = parse_anthropic_messages_payload(response.body, "response");
                    LLMResponse result;
                    result.stop_reason = map_stop_reason(payload.value("stop_reason", std::string{"end_turn"}));

                    if (!payload.contains("content") || !payload["content"].is_array()) {
                        return result;
                    }

                    for (const auto &block : payload["content"]) {
                        const auto type = block.value("type", std::string{});
                        if (type == "text") {
                            result.content.emplace_back(Text{block["text"].get<std::string>()});
                        } else if (type == "thinking") {
                            result.content.emplace_back(Thinking{block["thinking"].get<std::string>()});
                        } else if (type == "tool_use") {
                            result.content.emplace_back(ToolUse(block["id"].get<std::string>(), block["name"].get<std::string>(), block["input"]));
                        }
                    }

                    return result;
                } catch (const ProviderError &) {
                    throw;
                } catch (const nlohmann::json::exception &error) {
                    throw make_anthropic_messages_protocol_error("response", error.what());
                }
            }

            [[nodiscard]]
            std::unique_ptr<StreamDecoder> make_stream_decoder(const ProviderEventSink &sink) const override {
                return std::make_unique<AnthropicMessagesStreamDecoder>(sink);
            }

            [[nodiscard]]
            std::string label() const override {
                return "anthropic";
            }
        };

    } // namespace

    std::shared_ptr<const ProtocolAdapter> make_anthropic_messages_adapter() {
        static const auto adapter = std::make_shared<AnthropicMessagesAdapter>();
        return adapter;
    }

} // namespace orangutan::providers::protocols
