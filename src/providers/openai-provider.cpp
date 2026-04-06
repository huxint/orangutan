#include "providers/openai-provider.hpp"
#include "providers/http-client.hpp"
#include "providers/openai-protocol.hpp"
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

        // Build a user message with image_url parts for chat completions
        nlohmann::json build_image_user_message(const ToolResult &result) {
            nlohmann::json content = nlohmann::json::array();
            for (const auto &img : result.images) {
                content.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", "data:" + img.media_type + ";base64," + img.data}}},
                });
            }
            return {{"role", "user"}, {"content", content}};
        }

        // Build input_image items for the responses API
        nlohmann::json build_responses_image_input(const ToolResult &result) {
            nlohmann::json content = nlohmann::json::array();
            for (const auto &img : result.images) {
                content.push_back({
                    {"type", "input_image"},
                    {"image_url", "data:" + img.media_type + ";base64," + img.data},
                });
            }
            return {{"role", "user"}, {"content", content}};
        }

        constexpr std::string_view RESPONSES_ENDPOINT_STYLE = "openai-responses";
        constexpr std::string_view RESPONSES_API_PATH = "/v1/responses";
        constexpr std::string_view CHAT_COMPLETIONS_API_PATH = "/v1/chat/completions";

        [[nodiscard]]
        bool uses_responses_api(std::string_view endpoint_style) noexcept {
            return endpoint_style == RESPONSES_ENDPOINT_STYLE;
        }

        [[nodiscard]]
        std::string openai_request_url(const ProviderEndpoint &endpoint) {
            const auto path = uses_responses_api(endpoint.endpoint_style) ? RESPONSES_API_PATH : CHAT_COMPLETIONS_API_PATH;
            return endpoint.base_url + std::string{path};
        }

        [[nodiscard]]
        CurlHeaders make_openai_headers(const ProviderEndpoint &endpoint) {
            return compose_headers(endpoint.headers,
                                   {HeaderFallback{.key = "Content-Type", .fallback = "application/json"},
                                    HeaderFallback{.key = "Authorization", .fallback = std::string{"Bearer "} + endpoint.api_key}});
        }

        template <typename Converter>
        void append_tool_definitions(nlohmann::json &body, const std::vector<ToolDef> &tools, Converter &&converter) {
            if (tools.empty()) {
                return;
            }

            auto convert_tool = std::forward<Converter>(converter);
            auto &serialized_tools = body["tools"];
            serialized_tools = nlohmann::json::array();
            for (const auto &tool : tools) {
                serialized_tools.push_back(convert_tool(tool));
            }
        }

        void throw_if_openai_error(const nlohmann::json &response_json) {
            if (!response_json.contains("error")) {
                return;
            }

            std::string error_message = "API error";
            if (response_json["error"].contains("message")) {
                error_message = response_json["error"]["message"].get<std::string>();
            }
            throw std::runtime_error("API error: " + error_message);
        }

        class ChatCompletionsStreamAccumulator final : public JsonSseAccumulator<ChatCompletionsStreamAccumulator> {
        public:
            explicit ChatCompletionsStreamAccumulator(const StreamCallback &on_event)
            : on_event_(on_event) {}

            [[nodiscard]]
            static std::string_view parse_error_context() {
                return "OpenAI SSE data";
            }

            void handle_parsed_payload(const nlohmann::json &event_data) {
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
                    if (auto tool_use = detail::finalize_openai_tool_call(tc, "OpenAI chat completions stream"); tool_use.has_value()) {
                        response.content.emplace_back(std::move(*tool_use));
                    }
                }
                return response;
            }

        private:
            using ToolCallState = detail::OpenAiToolCallState;

            std::reference_wrapper<const StreamCallback> on_event_;
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
                on_event_.get()("text_delta", {{"text", text}});
            }

            void handle_reasoning_delta(const nlohmann::json &delta) {
                if (!delta.contains("reasoning_content") || delta["reasoning_content"].is_null()) {
                    return;
                }
                const auto text = delta["reasoning_content"].get<std::string>();
                reasoning_content_ += text;
                on_event_.get()("thinking_delta", {{"thinking", text}});
            }

            void handle_tool_call_deltas(const nlohmann::json &delta) {
                if (!delta.contains("tool_calls")) {
                    return;
                }

                for (const auto &tc : delta["tool_calls"]) {
                    if (!tc.contains("index") || !tc["index"].is_number_integer()) {
                        continue;
                    }
                    const auto index = tc["index"].get<std::size_t>();
                    while (tool_calls_.size() <= index) {
                        tool_calls_.push_back({});
                    }

                    auto &call = tool_calls_[index];
                    detail::merge_chat_completions_tool_call_delta(call, tc);

                    if (!call.announced && !call.id.empty() && !call.name.empty()) {
                        on_event_.get()("tool_call_start", {{"id", call.id}, {"name", call.name}, {"input", nlohmann::json::object()}});
                        call.announced = true;
                    }
                }
            }
        };

        class ResponsesStreamAccumulator final : public JsonSseAccumulator<ResponsesStreamAccumulator> {
        public:
            explicit ResponsesStreamAccumulator(const StreamCallback &on_event)
            : on_event_(on_event) {}

            [[nodiscard]]
            static std::string_view parse_error_context() {
                return "Responses SSE data";
            }

            void handle_parsed_payload(std::string_view event_name, const nlohmann::json &payload) {
                if (event_name == "response.output_text.delta") {
                    const auto delta = payload.value("delta", std::string{});
                    text_content_ += delta;
                    on_event_.get()("text_delta", {{"text", delta}});
                    return;
                }
                if (event_name == "response.reasoning_summary_text.delta" || event_name == "response.reasoning.delta") {
                    const auto delta = payload.value("delta", std::string{});
                    reasoning_content_ += delta;
                    on_event_.get()("thinking_delta", {{"thinking", delta}});
                    return;
                }
                if (event_name == "response.output_item.added") {
                    const auto item = payload.contains("item") ? payload["item"] : nlohmann::json::object();
                    if (item.value("type", std::string{}) == "function_call") {
                        auto &call = tool_calls_[item.value("id", item.value("call_id", std::string{}))];
                        call.id = item.value("call_id", item.value("id", std::string{}));
                        call.name = item.value("name", std::string{});
                        if (!call.announced && !call.id.empty() && !call.name.empty()) {
                            on_event_.get()("tool_call_start", {{"id", call.id}, {"name", call.name}, {"input", nlohmann::json::object()}});
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
                    if (auto tool_use = detail::finalize_openai_tool_call(call, "OpenAI responses stream"); tool_use.has_value()) {
                        response.content.emplace_back(std::move(*tool_use));
                    }
                }
                return response;
            }

        private:
            using ToolCallState = detail::OpenAiToolCallState;

            std::reference_wrapper<const StreamCallback> on_event_;
            std::string text_content_;
            std::string reasoning_content_;
            std::unordered_map<std::string, ToolCallState> tool_calls_;
            std::string stop_reason_;
        };

    } // namespace

    OpenAiProvider::OpenAiProvider(ProviderEndpoint endpoint)
    : endpoint_(std::move(endpoint)) {}

    std::optional<nlohmann::json> OpenAiProvider::message_to_openai(const Message &msg) {
        if (msg.role() == base::role::assistant) {
            return detail::serialize_openai_assistant_message(msg);
        }

        if (msg.role() == base::role::user) {
            for (const auto &block : msg) {
                if (const auto *result = std::get_if<ToolResult>(&block)) {
                    if (result->tool_use_id.empty()) {
                        spdlog::warn("Skipping tool result with empty tool_call_id while serializing history");
                        continue;
                    }
                    return nlohmann::json{{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}};
                }
            }

            std::string text;
            for (const auto &block : msg) {
                if (const auto *tb = std::get_if<Text>(&block)) {
                    text += tb->text;
                }
            }
            if (text.empty()) {
                return std::nullopt;
            }
            return nlohmann::json{{"role", "user"}, {"content", text}};
        }

        return nlohmann::json{{"role", magic_enum::enum_name(msg.role())}, {"content", ""}};
    }

    nlohmann::json OpenAiProvider::build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens,
                                                      bool stream, int thinking_budget) const {
        const auto resolved_max_tokens = endpoint_.default_max_tokens.value_or(max_tokens);
        static_cast<void>(thinking_budget);
        if (uses_responses_api(endpoint_.endpoint_style)) {
            return build_responses_request_body(system_prompt, messages, tools, resolved_max_tokens, stream);
        }
        return build_chat_completions_request_body(system_prompt, messages, tools, resolved_max_tokens, stream);
    }

    nlohmann::json OpenAiProvider::build_responses_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools,
                                                                 int resolved_max_tokens, bool stream) const {
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
            if (!converted.has_value()) {
                continue;
            }
            // function_call_output is a top-level input item, not nested in a message
            if (converted->contains("tool_call_id")) {
                body["input"].push_back({
                    {"type", "function_call_output"},
                    {"call_id", (*converted)["tool_call_id"]},
                    {"output", converted->value("content", std::string{})},
                });
                // Inject images from the original message as a follow-up user input
                for (const auto &block : msg) {
                    if (const auto *result = std::get_if<ToolResult>(&block); (result != nullptr) && !result->images.empty()) {
                        body["input"].push_back(build_responses_image_input(*result));
                    }
                }
                continue;
            }

            const auto role = converted->value("role", "user");

            // Emit text content as a simple message
            std::string content;
            if (converted->contains("content") && (*converted)["content"].is_string()) {
                content = (*converted)["content"].get<std::string>();
            }
            if (!content.empty()) {
                body["input"].push_back({
                    {"role", role},
                    {"content", content},
                });
            }

            // function_call items are top-level input items
            if (converted->contains("tool_calls")) {
                for (const auto &tool_call : (*converted)["tool_calls"]) {
                    body["input"].push_back({
                        {"type", "function_call"},
                        {"call_id", tool_call["id"]},
                        {"name", tool_call["function"]["name"]},
                        {"arguments", tool_call["function"]["arguments"]},
                    });
                }
            }
        }

        append_tool_definitions(body, tools, response_tool_to_json);
        return body;
    }

    nlohmann::json OpenAiProvider::build_chat_completions_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools,
                                                                        int resolved_max_tokens, bool stream) const {
        nlohmann::json body;
        body["model"] = endpoint_.model;
        body["max_tokens"] = resolved_max_tokens;
        if (stream) {
            body["stream"] = true;
        }
        if (endpoint_.thinking != "none" && !endpoint_.thinking.empty()) {
            body["reasoning_effort"] = endpoint_.thinking;
        }

        body["messages"] = nlohmann::json::array();
        if (!system_prompt.empty()) {
            body["messages"].push_back({{"role", "system"}, {"content", system_prompt}});
        }

        for (const auto &msg : messages) {
            if (msg.role() == base::role::user) {
                bool saw_tool_results = false;
                bool appended_tool_results = false;
                std::string user_text;

                for (const auto &block : msg) {
                    if (const auto *result = std::get_if<ToolResult>(&block)) {
                        saw_tool_results = true;
                        if (result->tool_use_id.empty()) {
                            spdlog::warn("Skipping tool result with empty tool_call_id while serializing history");
                            continue;
                        }
                        appended_tool_results = true;
                        body["messages"].push_back({{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}});
                        if (!result->images.empty()) {
                            body["messages"].push_back(build_image_user_message(*result));
                        }
                    } else if (const auto *tb = std::get_if<Text>(&block)) {
                        user_text += tb->text;
                    }
                }

                if (!user_text.empty() && appended_tool_results) {
                    body["messages"].push_back({{"role", "user"}, {"content", user_text}});
                } else if (saw_tool_results) {
                    if (!user_text.empty()) {
                        body["messages"].push_back({{"role", "user"}, {"content", user_text}});
                    }
                } else if (auto converted = message_to_openai(msg); converted.has_value()) {
                    body["messages"].push_back(std::move(*converted));
                }
            } else {
                if (auto converted = message_to_openai(msg); converted.has_value()) {
                    body["messages"].push_back(std::move(*converted));
                }
            }
        }

        append_tool_definitions(body, tools, tool_to_openai_function);
        return body;
    }

    LLMResponse OpenAiProvider::chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, false, thinking_budget);
        const auto request_body = body.dump();
        spdlog::debug("OpenAI request body: {}", request_body);

        auto headers = make_openai_headers(endpoint_);
        const std::string url = openai_request_url(endpoint_);
        auto response_body = http_post(url, request_body, headers);
        auto resp = nlohmann::json::parse(response_body);

        throw_if_openai_error(resp);
        if (uses_responses_api(endpoint_.endpoint_style)) {
            return parse_responses_response(resp);
        }
        return parse_chat_completions_response(resp);
    }

    LLMResponse OpenAiProvider::chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                            int max_tokens, int thinking_budget) {
        auto body = build_request_body(system_prompt, messages, tools, max_tokens, true, thinking_budget);
        const auto request_body = body.dump();
        spdlog::debug("OpenAI stream request body: {}", request_body);

        auto headers = make_openai_headers(endpoint_);
        const std::string url = openai_request_url(endpoint_);

        if (uses_responses_api(endpoint_.endpoint_style)) {
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
                detail::OpenAiToolCallState state;
                detail::merge_chat_completions_tool_call_delta(state, tc);
                if (auto tool_use = detail::finalize_openai_tool_call(state, "OpenAI chat completions response"); tool_use.has_value()) {
                    result.content.emplace_back(std::move(*tool_use));
                }
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
                detail::OpenAiToolCallState state{
                    .id = item.value("call_id", item.value("id", std::string{})),
                    .name = item.value("name", std::string{}),
                    .arguments = item.value("arguments", std::string{}),
                };
                if (auto tool_use = detail::finalize_openai_tool_call(state, "OpenAI responses response"); tool_use.has_value()) {
                    result.content.emplace_back(std::move(*tool_use));
                }
            }
        }

        return result;
    }

} // namespace orangutan::providers
