#include "core/providers/openai-provider.hpp"
#include "core/providers/http.hpp"
#include "core/providers/sse-parser.hpp"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {

    // ── OpenAI Stream Accumulator ──────────────────

    static std::string map_finish_reason(const std::string &reason) {
        if (reason == "stop") {
            return "end_turn";
        }
        if (reason == "tool_calls") {
            return "tool_use";
        }
        if (reason == "length") {
            return "max_tokens";
        }
        return reason;
    }

    class OpenAiStreamAccumulator {
    public:
        explicit OpenAiStreamAccumulator(const StreamCallback &on_event)
        : on_event_(on_event) {}

        void handle_event(const std::string &data) {
            if (data == "[DONE]") {
                return;
            }

            nlohmann::json event_data;
            try {
                event_data = nlohmann::json::parse(data);
            } catch (const nlohmann::json::parse_error &e) {
                spdlog::warn("Failed to parse OpenAI SSE data: {}", e.what());
                return;
            }

            if (!event_data.contains("choices") || event_data["choices"].empty()) {
                return;
            }

            const auto &choice = event_data["choices"][0];
            handle_finish_reason(choice);

            if (choice.contains("delta")) {
                handle_text_delta(choice["delta"]);
                handle_reasoning_delta(choice["delta"]);
                handle_tool_call_deltas(choice["delta"]);
            }
        }

        [[nodiscard]]
        LLMResponse build_response() const {
            LLMResponse response;
            response.stop_reason = stop_reason_.empty() ? "end_turn" : stop_reason_;

            if (!reasoning_content_.empty()) {
                response.content.emplace_back(Thinking{reasoning_content_});
            }

            if (!text_content_.empty()) {
                response.content.emplace_back(Text{text_content_});
            }

            for (const auto &tc : tool_calls_) {
                nlohmann::json input = nlohmann::json::object();
                if (!tc.arguments.empty()) {
                    try {
                        input = nlohmann::json::parse(tc.arguments);
                    } catch (const nlohmann::json::parse_error &) {
                        spdlog::warn("Failed to parse OpenAI tool call arguments");
                    }
                }
                response.content.emplace_back(ToolUse(tc.id, tc.name, std::move(input)));
            }

            return response;
        }

    private:
        struct ToolCallState {
            std::string id;
            std::string name;
            std::string arguments;
            bool announced = false;
        };

        const StreamCallback &on_event_;
        std::string text_content_;
        std::string reasoning_content_;
        std::vector<ToolCallState> tool_calls_;
        std::string stop_reason_;

        void handle_finish_reason(const nlohmann::json &choice) {
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                stop_reason_ = map_finish_reason(choice["finish_reason"].get<std::string>());
            }
        }

        void handle_text_delta(const nlohmann::json &delta) {
            if (!delta.contains("content") || delta["content"].is_null()) {
                return;
            }
            auto text = delta["content"].get<std::string>();
            text_content_ += text;

            nlohmann::json delta_event;
            delta_event["text"] = text;
            on_event_("text_delta", delta_event);
        }

        void handle_reasoning_delta(const nlohmann::json &delta) {
            if (!delta.contains("reasoning_content") || delta["reasoning_content"].is_null()) {
                return;
            }
            auto text = delta["reasoning_content"].get<std::string>();
            reasoning_content_ += text;
            on_event_("thinking_delta", {{"thinking", text}});
        }

        void handle_tool_call_deltas(const nlohmann::json &delta) {
            if (!delta.contains("tool_calls")) {
                return;
            }

            for (const auto &tc : delta["tool_calls"]) {
                auto index = tc["index"].get<std::size_t>();

                while (tool_calls_.size() <= index) {
                    tool_calls_.push_back({});
                }

                auto &call = tool_calls_[index];

                if (tc.contains("id")) {
                    call.id = tc["id"].get<std::string>();
                }
                if (tc.contains("function")) {
                    if (tc["function"].contains("name")) {
                        call.name = tc["function"]["name"].get<std::string>();
                    }
                    if (tc["function"].contains("arguments")) {
                        call.arguments += tc["function"]["arguments"].get<std::string>();
                    }
                }

                if (!call.announced && !call.id.empty() && !call.name.empty()) {
                    nlohmann::json event;
                    event["id"] = call.id;
                    event["name"] = call.name;
                    event["input"] = nlohmann::json::object();
                    on_event_("tool_call_start", event);
                    call.announced = true;
                }
            }
        }
    };

    // ── OpenAiProvider ─────────────────────────────

    OpenAiProvider::OpenAiProvider(std::string api_key, std::string model, std::string base_url)
    : api_key_(std::move(api_key)),
      model_(std::move(model)),
      base_url_(std::move(base_url)) {}

    nlohmann::json OpenAiProvider::message_to_openai(const Message &msg) {
        // Handle assistant messages with tool calls
        if (msg.role() == base::role::assistant) {
            nlohmann::json j;
            j["role"] = "assistant";

            std::string text_content;
            nlohmann::json tool_calls = nlohmann::json::array();

            for (const auto &block : msg) {
                if (std::get_if<Thinking>(&block) != nullptr) {
                    continue; // OpenAI does not accept reasoning blocks in requests
                }
                if (const auto *text = std::get_if<Text>(&block)) {
                    text_content += text->text;
                } else if (const auto *tool = std::get_if<ToolUse>(&block)) {
                    tool_calls.push_back({{"id", tool->id}, {"type", "function"}, {"function", {{"name", tool->name}, {"arguments", tool->input.dump()}}}});
                }
            }

            if (!text_content.empty()) {
                j["content"] = text_content;
            } else {
                j["content"] = nullptr;
            }

            if (!tool_calls.empty()) {
                j["tool_calls"] = tool_calls;
            }

            return j;
        }

        // Handle user messages with tool results
        if (msg.role() == base::role::user) {
            // Check if this contains tool results
            for (const auto &block : msg) {
                if (const auto *result = std::get_if<ToolResult>(&block)) {
                    nlohmann::json j;
                    j["role"] = "tool";
                    j["tool_call_id"] = result->tool_use_id;
                    j["content"] = result->content;
                    return j;
                }
            }

            // Regular user message — combine text blocks
            std::string text;
            for (const auto &block : msg) {
                if (const auto *tb = std::get_if<Text>(&block)) {
                    text += tb->text;
                }
            }
            return {{"role", "user"}, {"content", text}};
        }

        // Fallback
        return {{"role", magic_enum::enum_name(msg.role())}, {"content", ""}};
    }

    nlohmann::json OpenAiProvider::build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                                      bool stream) const {
        nlohmann::json body;
        body["model"] = model_;
        body["max_tokens"] = max_tokens;

        if (stream) {
            body["stream"] = true;
        }

        // Build messages array — system prompt is a message in OpenAI format
        body["messages"] = nlohmann::json::array();
        if (!system_prompt.empty()) {
            body["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
        }

        for (const auto &msg : messages) {
            // A user message might contain multiple tool results — each becomes a separate message
            if (msg.role() == base::role::user) {
                bool has_tool_results = false;
                std::string user_text;

                for (const auto &block : msg) {
                    if (const auto *result = std::get_if<ToolResult>(&block)) {
                        has_tool_results = true;
                        nlohmann::json tool_msg;
                        tool_msg["role"] = "tool";
                        tool_msg["tool_call_id"] = result->tool_use_id;
                        tool_msg["content"] = result->content;
                        body["messages"].push_back(std::move(tool_msg));
                    } else if (const auto *tb = std::get_if<Text>(&block)) {
                        user_text += tb->text;
                    }
                }

                // If there were also text blocks alongside tool results, add them separately
                if (!user_text.empty() && has_tool_results) {
                    body["messages"].push_back({{"role", "user"}, {"content", user_text}});
                } else if (!has_tool_results) {
                    body["messages"].push_back(message_to_openai(msg));
                }
            } else {
                body["messages"].push_back(message_to_openai(msg));
            }
        }

        // Tools in OpenAI format
        if (!tools.empty()) {
            body["tools"] = nlohmann::json::array();
            for (const auto &tool : tools) {
                body["tools"].push_back({{"type", "function"}, {"function", {{"name", tool.name}, {"description", tool.description}, {"parameters", tool.input_schema}}}});
            }
        }

        return body;
    }

    LLMResponse OpenAiProvider::chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                     int /*thinking_budget*/) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, false);
        std::string request_body = body.dump();
        spdlog::debug("OpenAI request body: {}", request_body);

        CurlHeaders headers;
        headers.append("Content-Type: application/json");
        headers.append("Authorization: Bearer " + api_key_);

        std::string url = base_url_ + "/v1/chat/completions";
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

    LLMResponse OpenAiProvider::chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                            int max_tokens, int /*thinking_budget*/) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, true);
        std::string request_body = body.dump();
        spdlog::debug("OpenAI stream request body: {}", request_body);

        OpenAiStreamAccumulator accumulator(on_event);

        SseParser parser([&](const std::string & /*event_name*/, const std::string &data) {
            accumulator.handle_event(data);
        });

        CurlHeaders headers;
        headers.append("Content-Type: application/json");
        headers.append("Authorization: Bearer " + api_key_);

        std::string url = base_url_ + "/v1/chat/completions";
        long http_code = http_post_stream(url, request_body, headers, parser);

        if (http_code != 200) {
            throw std::runtime_error("API error (HTTP " + std::to_string(http_code) + ")");
        }

        return accumulator.build_response();
    }

    LLMResponse OpenAiProvider::parse_response(const nlohmann::json &resp) {
        LLMResponse result;

        if (!resp.contains("choices") || resp["choices"].empty()) {
            return result;
        }

        const auto &choice = resp["choices"][0];

        // Map finish_reason to orangutan stop_reason
        result.stop_reason = map_finish_reason(choice.value("finish_reason", "stop"));

        const auto &message = choice["message"];

        // Reasoning content (OpenAI reasoning models)
        if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
            result.content.emplace_back(Thinking{message["reasoning_content"].get<std::string>()});
        }

        // Text content
        if (message.contains("content") && !message["content"].is_null()) {
            result.content.emplace_back(Text{message["content"].get<std::string>()});
        }

        // Tool calls
        if (message.contains("tool_calls")) {
            for (const auto &tc : message["tool_calls"]) {
                auto func = tc["function"];
                nlohmann::json input = nlohmann::json::object();
                if (func.contains("arguments") && !func["arguments"].is_null()) {
                    try {
                        input = nlohmann::json::parse(func["arguments"].get<std::string>());
                    } catch (const nlohmann::json::parse_error &) {
                        spdlog::warn("Failed to parse OpenAI tool call arguments");
                    }
                }
                result.content.emplace_back(ToolUse(tc["id"].get<std::string>(), func["name"].get<std::string>(), std::move(input)));
            }
        }

        return result;
    }

} // namespace orangutan
