#include "providers/openai-provider.hpp"
#include "providers/http-client.hpp"
#include "providers/sse-parser.hpp"

#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan::providers {

    namespace {

        std::string map_finish_reason(const std::string &reason) {
            if (reason == "stop" || reason == "completed") {
                return "end_turn";
            }
            if (reason == "tool_calls" || reason == "function_call") {
                return "tool_use";
            }
            if (reason == "length" || reason == "max_output_tokens") {
                return "max_tokens";
            }
            return reason;
        }

        void append_header_if_missing(CurlHeaders &headers, const std::unordered_map<std::string, std::string> &custom_headers, std::string_view key,
                                      std::string_view fallback_value) {
            if (!custom_headers.contains(static_cast<std::string>(key))) {
                headers.append(static_cast<std::string>(key) + ": " + static_cast<std::string>(fallback_value));
            }
        }

        void append_custom_headers(CurlHeaders &headers, const std::unordered_map<std::string, std::string> &custom_headers) {
            for (const auto &[name, value] : custom_headers) {
                auto header = name;
                header += ": ";
                header += value;
                headers.append(header);
            }
        }

        nlohmann::json tool_to_openai_function(const ToolDef &tool) {
            return {
                {"type", "function"},
                {"function",
                 {
                     {"name", tool.name},
                     {"description", tool.description},
                     {"parameters", tool.input_schema},
                 }},
            };
        }

        nlohmann::json response_tool_to_json(const ToolDef &tool) {
            return {
                {"type", "function"},
                {"name", tool.name},
                {"description", tool.description},
                {"parameters", tool.input_schema},
            };
        }

        class ChatCompletionsStreamAccumulator {
        public:
            explicit ChatCompletionsStreamAccumulator(const StreamCallback &on_event)
            : on_event_(on_event) {}

            void handle_data(const std::string &data) {
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
                const auto text = delta["content"].get<std::string>();
                text_content_ += text;
                on_event_("text_delta", {{"text", text}});
            }

            void handle_reasoning_delta(const nlohmann::json &delta) {
                if (!delta.contains("reasoning_content") || delta["reasoning_content"].is_null()) {
                    return;
                }
                const auto text = delta["reasoning_content"].get<std::string>();
                reasoning_content_ += text;
                on_event_("thinking_delta", {{"thinking", text}});
            }

            void handle_tool_call_deltas(const nlohmann::json &delta) {
                if (!delta.contains("tool_calls")) {
                    return;
                }

                for (const auto &tc : delta["tool_calls"]) {
                    const auto index = tc["index"].get<std::size_t>();
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
                        on_event_("tool_call_start", {{"id", call.id}, {"name", call.name}, {"input", nlohmann::json::object()}});
                        call.announced = true;
                    }
                }
            }
        };

        class ResponsesStreamAccumulator {
        public:
            explicit ResponsesStreamAccumulator(const StreamCallback &on_event)
            : on_event_(on_event) {}

            void handle_event(const std::string &event_name, const std::string &data) {
                if (data == "[DONE]") {
                    return;
                }

                nlohmann::json payload;
                try {
                    payload = nlohmann::json::parse(data);
                } catch (const nlohmann::json::parse_error &e) {
                    spdlog::warn("Failed to parse Responses SSE data: {}", e.what());
                    return;
                }

                if (event_name == "response.output_text.delta") {
                    const auto delta = payload.value("delta", std::string{});
                    text_content_ += delta;
                    on_event_("text_delta", {{"text", delta}});
                    return;
                }
                if (event_name == "response.reasoning_summary_text.delta" || event_name == "response.reasoning.delta") {
                    const auto delta = payload.value("delta", std::string{});
                    reasoning_content_ += delta;
                    on_event_("thinking_delta", {{"thinking", delta}});
                    return;
                }
                if (event_name == "response.output_item.added") {
                    const auto item = payload.contains("item") ? payload["item"] : nlohmann::json::object();
                    if (item.value("type", std::string{}) == "function_call") {
                        auto &call = tool_calls_[item.value("id", item.value("call_id", std::string{}))];
                        call.id = item.value("call_id", item.value("id", std::string{}));
                        call.name = item.value("name", std::string{});
                        if (!call.announced && !call.id.empty() && !call.name.empty()) {
                            on_event_("tool_call_start", {{"id", call.id}, {"name", call.name}, {"input", nlohmann::json::object()}});
                            call.announced = true;
                        }
                    }
                    return;
                }
                if (event_name == "response.function_call_arguments.delta") {
                    const auto item_id = payload.value("item_id", payload.value("call_id", std::string{}));
                    if (!item_id.empty()) {
                        tool_calls_[item_id].arguments += payload.value("delta", std::string{});
                    }
                    return;
                }
                if (event_name == "response.completed") {
                    if (payload.contains("response")) {
                        const auto &response = payload["response"];
                        const auto status = response.value("status", std::string{});
                        stop_reason_ = status.empty() ? "end_turn" : map_finish_reason(status);
                    }
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
                for (const auto &[id, call] : tool_calls_) {
                    static_cast<void>(id);
                    nlohmann::json input = nlohmann::json::object();
                    if (!call.arguments.empty()) {
                        try {
                            input = nlohmann::json::parse(call.arguments);
                        } catch (const nlohmann::json::parse_error &) {
                            spdlog::warn("Failed to parse Responses tool call arguments");
                        }
                    }
                    response.content.emplace_back(ToolUse(call.id, call.name, std::move(input)));
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
            std::unordered_map<std::string, ToolCallState> tool_calls_;
            std::string stop_reason_;
        };

    } // namespace

    OpenAiProvider::OpenAiProvider(ProviderEndpoint endpoint)
    : endpoint_(std::move(endpoint)) {}

    nlohmann::json OpenAiProvider::message_to_openai(const Message &msg) {
        if (msg.role() == base::role::assistant) {
            nlohmann::json j;
            j["role"] = "assistant";

            std::string text_content;
            nlohmann::json tool_calls = nlohmann::json::array();

            for (const auto &block : msg) {
                if (std::get_if<Thinking>(&block) != nullptr) {
                    continue;
                }
                if (const auto *text = std::get_if<Text>(&block)) {
                    text_content += text->text;
                } else if (const auto *tool = std::get_if<ToolUse>(&block)) {
                    tool_calls.push_back({{"id", tool->id}, {"type", "function"}, {"function", {{"name", tool->name}, {"arguments", tool->input.dump()}}}});
                }
            }

            j["content"] = text_content.empty() ? nlohmann::json(nullptr) : nlohmann::json(text_content);
            if (!tool_calls.empty()) {
                j["tool_calls"] = tool_calls;
            }
            return j;
        }

        if (msg.role() == base::role::user) {
            for (const auto &block : msg) {
                if (const auto *result = std::get_if<ToolResult>(&block)) {
                    return {{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}};
                }
            }

            std::string text;
            for (const auto &block : msg) {
                if (const auto *tb = std::get_if<Text>(&block)) {
                    text += tb->text;
                }
            }
            return {{"role", "user"}, {"content", text}};
        }

        return {{"role", magic_enum::enum_name(msg.role())}, {"content", ""}};
    }

    nlohmann::json OpenAiProvider::build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                                      bool stream, int /*thinking_budget*/) const {
        const auto resolved_max_tokens = endpoint_.default_max_tokens.value_or(max_tokens);

        if (endpoint_.endpoint_style == "openai-responses") {
            nlohmann::json body;
            body["model"] = endpoint_.model;
            body["max_output_tokens"] = resolved_max_tokens;
            if (!system_prompt.empty()) {
                body["instructions"] = system_prompt;
            }
            if (stream) {
                body["stream"] = true;
            }
            if (endpoint_.thinking != "none" && !endpoint_.thinking.empty()) {
                body["reasoning"] = {{"effort", endpoint_.thinking}};
            }

            body["input"] = nlohmann::json::array();
            for (const auto &msg : messages) {
                auto converted = message_to_openai(msg);
                nlohmann::json response_msg = {
                    {"role", converted.value("role", "user")},
                    {"content", nlohmann::json::array()},
                };
                if (converted.contains("tool_call_id")) {
                    response_msg["content"].push_back({
                        {"type", "function_call_output"},
                        {"call_id", converted["tool_call_id"]},
                        {"output", converted.value("content", std::string{})},
                    });
                } else {
                    const auto content = converted.value("content", std::string{});
                    response_msg["content"].push_back({
                        {"type", response_msg["role"] == "assistant" ? "output_text" : "input_text"},
                        {"text", content},
                    });
                    if (converted.contains("tool_calls")) {
                        for (const auto &tool_call : converted["tool_calls"]) {
                            response_msg["content"].push_back({
                                {"type", "function_call"},
                                {"call_id", tool_call["id"]},
                                {"name", tool_call["function"]["name"]},
                                {"arguments", tool_call["function"]["arguments"]},
                            });
                        }
                    }
                }
                body["input"].push_back(std::move(response_msg));
            }

            if (!tools.empty()) {
                body["tools"] = nlohmann::json::array();
                for (const auto &tool : tools) {
                    body["tools"].push_back(response_tool_to_json(tool));
                }
            }
            return body;
        }

        nlohmann::json body;
        body["model"] = endpoint_.model;
        body["max_tokens"] = resolved_max_tokens;
        if (stream) {
            body["stream"] = true;
        }

        body["messages"] = nlohmann::json::array();
        if (!system_prompt.empty()) {
            body["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
        }

        for (const auto &msg : messages) {
            if (msg.role() == base::role::user) {
                bool has_tool_results = false;
                std::string user_text;

                for (const auto &block : msg) {
                    if (const auto *result = std::get_if<ToolResult>(&block)) {
                        has_tool_results = true;
                        body["messages"].push_back({{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}});
                    } else if (const auto *tb = std::get_if<Text>(&block)) {
                        user_text += tb->text;
                    }
                }

                if (!user_text.empty() && has_tool_results) {
                    body["messages"].push_back({{"role", "user"}, {"content", user_text}});
                } else if (!has_tool_results) {
                    body["messages"].push_back(message_to_openai(msg));
                }
            } else {
                body["messages"].push_back(message_to_openai(msg));
            }
        }

        if (!tools.empty()) {
            body["tools"] = nlohmann::json::array();
            for (const auto &tool : tools) {
                body["tools"].push_back(tool_to_openai_function(tool));
            }
        }

        return body;
    }

    LLMResponse OpenAiProvider::chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, false, thinking_budget);
        const auto request_body = body.dump();
        spdlog::debug("OpenAI request body: {}", request_body);

        CurlHeaders headers;
        append_header_if_missing(headers, endpoint_.headers, "Content-Type", "application/json");
        append_header_if_missing(headers, endpoint_.headers, "Authorization", std::string{"Bearer "} + endpoint_.api_key);
        append_custom_headers(headers, endpoint_.headers);

        const std::string url = endpoint_.base_url + (endpoint_.endpoint_style == "openai-responses" ? "/v1/responses" : "/v1/chat/completions");
        auto response_body = http_post(url, request_body, headers);
        auto resp = nlohmann::json::parse(response_body);

        if (resp.contains("error")) {
            std::string err_msg = "API error";
            if (resp["error"].contains("message")) {
                err_msg = resp["error"]["message"].get<std::string>();
            }
            throw std::runtime_error("API error: " + err_msg);
        }

        if (endpoint_.endpoint_style == "openai-responses") {
            return parse_responses_response(resp);
        }
        return parse_chat_completions_response(resp);
    }

    LLMResponse OpenAiProvider::chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                            int max_tokens, int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, true, thinking_budget);
        const auto request_body = body.dump();
        spdlog::debug("OpenAI stream request body: {}", request_body);

        CurlHeaders headers;
        append_header_if_missing(headers, endpoint_.headers, "Content-Type", "application/json");
        append_header_if_missing(headers, endpoint_.headers, "Authorization", std::string{"Bearer "} + endpoint_.api_key);
        append_custom_headers(headers, endpoint_.headers);

        const std::string url = endpoint_.base_url + (endpoint_.endpoint_style == "openai-responses" ? "/v1/responses" : "/v1/chat/completions");

        if (endpoint_.endpoint_style == "openai-responses") {
            ResponsesStreamAccumulator accumulator(on_event);
            SseParser parser([&](const std::string &event_name, const std::string &data) {
                accumulator.handle_event(event_name, data);
            });
            const long http_code = http_post_stream(url, request_body, headers, parser);
            if (http_code != 200) {
                throw std::runtime_error("API error (HTTP " + std::to_string(http_code) + ")");
            }
            return accumulator.build_response();
        }

        ChatCompletionsStreamAccumulator accumulator(on_event);
        SseParser parser([&](const std::string & /*event_name*/, const std::string &data) {
            accumulator.handle_data(data);
        });
        const long http_code = http_post_stream(url, request_body, headers, parser);
        if (http_code != 200) {
            throw std::runtime_error("API error (HTTP " + std::to_string(http_code) + ")");
        }
        return accumulator.build_response();
    }

    LLMResponse OpenAiProvider::parse_chat_completions_response(const nlohmann::json &resp) {
        LLMResponse result;
        if (!resp.contains("choices") || resp["choices"].empty()) {
            return result;
        }

        const auto &choice = resp["choices"][0];
        result.stop_reason = map_finish_reason(choice.value("finish_reason", "stop"));
        const auto &message = choice["message"];

        if (message.contains("reasoning_content") && !message["reasoning_content"].is_null()) {
            result.content.emplace_back(Thinking{message["reasoning_content"].get<std::string>()});
        }
        if (message.contains("content") && !message["content"].is_null()) {
            result.content.emplace_back(Text{message["content"].get<std::string>()});
        }
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

    LLMResponse OpenAiProvider::parse_responses_response(const nlohmann::json &resp) {
        LLMResponse result;
        result.stop_reason = map_finish_reason(resp.value("status", "completed"));

        if (resp.contains("output_text") && resp["output_text"].is_string() && !resp["output_text"].get<std::string>().empty()) {
            result.content.emplace_back(Text{resp["output_text"].get<std::string>()});
        }

        if (!resp.contains("output") || !resp["output"].is_array()) {
            return result;
        }

        for (const auto &item : resp["output"]) {
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
                nlohmann::json input = nlohmann::json::object();
                if (item.contains("arguments") && item["arguments"].is_string()) {
                    try {
                        input = nlohmann::json::parse(item["arguments"].get<std::string>());
                    } catch (const nlohmann::json::parse_error &) {
                        spdlog::warn("Failed to parse Responses tool call arguments");
                    }
                }
                result.content.emplace_back(ToolUse(item.value("call_id", item.value("id", std::string{})), item.value("name", std::string{}), std::move(input)));
            }
        }

        return result;
    }

} // namespace orangutan::providers
