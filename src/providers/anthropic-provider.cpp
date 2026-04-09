#include "providers/anthropic-provider.hpp"
#include "providers/sse-parser.hpp"
#include "providers/http-client.hpp"
#include "types/serialization.hpp"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan::providers {

    namespace {

        int anthropic_thinking_budget(std::string_view thinking) {
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

    } // namespace

    // ── StreamAccumulator ───────────────────────────

    class StreamAccumulator final : public JsonSseAccumulator<StreamAccumulator> {
    public:
        explicit StreamAccumulator(const StreamCallback &on_event)
        : on_event_(&on_event) {}

        void handle_event(std::string_view data) {
            handle_data(data);
        }

        [[nodiscard]]
        static std::string_view parse_error_context() {
            return "SSE data";
        }

        void handle_parsed_payload(const nlohmann::json &event_data) {
            auto type = event_data.value("type", "");
            dispatch_event(type, event_data);
            (*on_event_)(type, event_data);
        }

        [[nodiscard]]
        LLMResponse build_response() const {
            LLMResponse response;
            response.stop_reason = stop_reason_.empty() ? "end_turn" : stop_reason_;

            for (const auto &block : blocks_) {
                if (block.type == "text") {
                    response.content.emplace_back(Text{block.text});
                } else if (block.type == "thinking") {
                    response.content.emplace_back(Thinking{block.text});
                } else if (block.type == "tool_use") {
                    nlohmann::json input = nlohmann::json::object();
                    if (!block.input_json.empty()) {
                        try {
                            input = nlohmann::json::parse(block.input_json);
                        } catch (const nlohmann::json::parse_error &) {
                            spdlog::warn("Failed to parse accumulated tool input JSON");
                        }
                    }
                    response.content.emplace_back(ToolUse(block.id, block.name, std::move(input)));
                }
            }

            return response;
        }

    private:
        struct BlockState {
            std::string type;
            std::string text;
            std::string id;
            std::string name;
            std::string input_json;
        };

        const StreamCallback *on_event_;
        std::vector<BlockState> blocks_;
        std::string stop_reason_;

        void dispatch_event(const std::string &type, const nlohmann::json &event_data) {
            if (type == "content_block_start") {
                handle_block_start(event_data);
            } else if (type == "content_block_delta") {
                handle_block_delta(event_data);
            } else if (type == "message_delta") {
                handle_message_delta(event_data);
            }
        }

        void handle_block_start(const nlohmann::json &event_data) {
            auto block = event_data["content_block"];
            auto block_type = block["type"].get<std::string>();

            BlockState state;
            state.type = block_type;
            if (block_type == "tool_use") {
                state.id = block.value("id", "");
                state.name = block.value("name", "");
                (*on_event_)("tool_call_start", {
                                                    {"id", state.id},
                                                    {"name", state.name},
                                                    {"input", nlohmann::json::object()},
                                                });
            }
            blocks_.push_back(std::move(state));
        }

        void handle_block_delta(const nlohmann::json &event_data) {
            auto index = event_data["index"].get<std::size_t>();
            if (index >= blocks_.size()) {
                return;
            }

            auto &block = blocks_[index];
            auto delta = event_data["delta"];
            auto delta_type = delta["type"].get<std::string>();

            if (delta_type == "text_delta") {
                block.text += delta["text"].get<std::string>();
                (*on_event_)("text_delta", delta);
            } else if (delta_type == "input_json_delta") {
                block.input_json += delta["partial_json"].get<std::string>();
            } else if (delta_type == "thinking_delta") {
                block.text += delta["thinking"].get<std::string>();
                (*on_event_)("thinking_delta", {{"thinking", delta["thinking"]}});
            }
        }

        void handle_message_delta(const nlohmann::json &event_data) {
            if (event_data.contains("delta") && event_data["delta"].contains("stop_reason")) {
                stop_reason_ = event_data["delta"]["stop_reason"].get<std::string>();
            }
        }
    };

    // ── AnthropicProvider ───────────────────────────

    AnthropicProvider::AnthropicProvider(ProviderEndpoint endpoint)
    : endpoint_(std::move(endpoint)) {}

    nlohmann::json AnthropicProvider::build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                                         bool stream, int thinking_budget) const {
        nlohmann::json body;
        const auto resolved_max_tokens = endpoint_.default_max_tokens.value_or(max_tokens);
        body["model"] = endpoint_.model;
        body["max_tokens"] = resolved_max_tokens;
        body["system"] = system_prompt;

        if (stream) {
            body["stream"] = true;
        }

        body["messages"] = nlohmann::json::array();
        for (const auto &msg : messages) {
            body["messages"].push_back(message_to_json(msg));
        }

        if (!tools.empty()) {
            body["tools"] = nlohmann::json::array();
            for (const auto &tool : tools) {
                body["tools"].push_back({{"name", tool.name}, {"description", tool.description}, {"input_schema", tool.input_schema}});
            }
        }

        const auto resolved_thinking_budget = thinking_budget > 0 ? thinking_budget : anthropic_thinking_budget(endpoint_.thinking);
        if (resolved_thinking_budget > 0) {
            body["thinking"] = {{"type", "enabled"}, {"budget_tokens", resolved_thinking_budget}};
        }

        return body;
    }

    LLMResponse AnthropicProvider::chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                        int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, false, thinking_budget);
        std::string request_body = body.dump();
        spdlog::debug("Request body: {}", request_body);

        auto headers = compose_headers(endpoint_.headers,
                                       {HeaderFallback{.key = "Content-Type", .fallback = "application/json"}, HeaderFallback{.key = "x-api-key", .fallback = endpoint_.api_key},
                                        HeaderFallback{.key = "anthropic-version", .fallback = "2023-06-01"}});

        std::string url = endpoint_.base_url + "/v1/messages";
        auto response_body = http_post(url, request_body, headers);

        nlohmann::json resp = nlohmann::json::parse(response_body);

        if (resp.contains("error")) {
            std::string err_msg = "API error";
            if (resp["error"].contains("message")) {
                err_msg = resp["error"]["message"].get<std::string>();
            }
            throw std::runtime_error("API error: " + err_msg);
        }

        return parse_response(resp);
    }

    LLMResponse AnthropicProvider::chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools,
                                               const StreamCallback &on_event, int max_tokens, int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, true, thinking_budget);
        std::string request_body = body.dump();
        spdlog::debug("Stream request body: {}", request_body);

        StreamAccumulator accumulator(on_event);

        SseParser parser([&](const std::string & /*event_name*/, const std::string &data) {
            accumulator.handle_event(data);
        });

        auto headers = compose_headers(endpoint_.headers,
                                       {HeaderFallback{.key = "Content-Type", .fallback = "application/json"}, HeaderFallback{.key = "x-api-key", .fallback = endpoint_.api_key},
                                        HeaderFallback{.key = "anthropic-version", .fallback = "2023-06-01"}});

        std::string url = endpoint_.base_url + "/v1/messages";
        long http_code = http_post_stream(url, request_body, headers, parser);

        if (http_code != 200) {
            throw std::runtime_error("API error (HTTP " + std::to_string(http_code) + ")");
        }

        return accumulator.build_response();
    }

    LLMResponse AnthropicProvider::parse_response(const nlohmann::json &resp) {
        LLMResponse result;
        result.stop_reason = resp.value("stop_reason", "end_turn");

        for (const auto &block : resp["content"]) {
            std::string type = block["type"].get<std::string>();

            if (type == "text") {
                result.content.emplace_back(Text{block["text"].get<std::string>()});
            } else if (type == "thinking") {
                result.content.emplace_back(Thinking{block["thinking"].get<std::string>()});
            } else if (type == "tool_use") {
                result.content.emplace_back(ToolUse(block["id"].get<std::string>(), block["name"].get<std::string>(), block["input"]));
            }
        }

        return result;
    }

} // namespace orangutan::providers
