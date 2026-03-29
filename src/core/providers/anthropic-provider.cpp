#include "core/providers/anthropic-provider.hpp"
#include "core/providers/sse-parser.hpp"
#include "core/providers/http.hpp"
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {

    // ── StreamAccumulator ───────────────────────────

    class StreamAccumulator {
    public:
        explicit StreamAccumulator(const StreamCallback &on_event)
        : on_event_(on_event) {}

        void handle_event(const std::string &data) {
            if (data == "[DONE]") {
                return;
            }

            nlohmann::json event_data;
            try {
                event_data = nlohmann::json::parse(data);
            } catch (const nlohmann::json::parse_error &e) {
                spdlog::warn("Failed to parse SSE data: {}", e.what());
                return;
            }

            auto type = event_data.value("type", "");
            dispatch_event(type, event_data);
            on_event_(type, event_data);
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

        const StreamCallback &on_event_;
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
                on_event_("tool_call_start", {
                                                 {"id", state.id},
                                                 {"name", state.name},
                                                 {"input", nlohmann::json::object()},
                                             });
            }
            blocks_.push_back(std::move(state));
        }

        void handle_block_delta(const nlohmann::json &event_data) {
            auto index = event_data["index"].get<size_t>();
            if (index >= blocks_.size()) {
                return;
            }

            auto &block = blocks_[index];
            auto delta = event_data["delta"];
            auto delta_type = delta["type"].get<std::string>();

            if (delta_type == "text_delta") {
                block.text += delta["text"].get<std::string>();
                on_event_("text_delta", delta);
            } else if (delta_type == "input_json_delta") {
                block.input_json += delta["partial_json"].get<std::string>();
            } else if (delta_type == "thinking_delta") {
                block.text += delta["thinking"].get<std::string>();
                on_event_("thinking_delta", {{"thinking", delta["thinking"]}});
            }
        }

        void handle_message_delta(const nlohmann::json &event_data) {
            if (event_data.contains("delta") && event_data["delta"].contains("stop_reason")) {
                stop_reason_ = event_data["delta"]["stop_reason"].get<std::string>();
            }
        }
    };

    // ── AnthropicProvider ───────────────────────────

    AnthropicProvider::AnthropicProvider(std::string api_key, std::string model, std::string base_url)
    : api_key_(std::move(api_key)),
      model_(std::move(model)),
      base_url_(std::move(base_url)) {}

    nlohmann::json AnthropicProvider::build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                                         bool stream, int thinking_budget) const {
        nlohmann::json body;
        body["model"] = model_;
        body["max_tokens"] = max_tokens;
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

        if (thinking_budget > 0) {
            body["thinking"] = {{"type", "enabled"}, {"budget_tokens", thinking_budget}};
        }

        return body;
    }

    LLMResponse AnthropicProvider::chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                        int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, false, thinking_budget);
        std::string request_body = body.dump();
        spdlog::debug("Request body: {}", request_body);

        CurlHeaders headers;
        headers.append("Content-Type: application/json");
        headers.append("x-api-key: " + api_key_);
        headers.append("anthropic-version: 2023-06-01");

        std::string url = base_url_ + "/v1/messages";
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

        CurlHeaders headers;
        headers.append("Content-Type: application/json");
        headers.append("x-api-key: " + api_key_);
        headers.append("anthropic-version: 2023-06-01");

        std::string url = base_url_ + "/v1/messages";
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

} // namespace orangutan
