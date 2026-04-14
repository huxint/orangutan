#include "providers/protocols/openai-common.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::providers::protocols::openai {
    namespace {

        void merge_tool_name(std::string &current, std::string_view delta) {
            if (delta.empty()) {
                return;
            }
            if (current.empty()) {
                current = delta;
                return;
            }
            if (delta == current || current.ends_with(delta)) {
                return;
            }
            if (delta.starts_with(current)) {
                current = delta;
                return;
            }
            current += delta;
        }

        [[nodiscard]]
        bool valid_tool_use(const ToolUse &tool_use) {
            return !tool_use.id.empty() && !tool_use.name.empty();
        }

        [[nodiscard]]
        nlohmann::json build_image_user_message(const ToolResult &result) {
            nlohmann::json content = nlohmann::json::array();
            for (const auto &image : result.images) {
                content.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", "data:" + image.media_type + ";base64," + image.data}}},
                });
            }
            return {{"role", "user"}, {"content", content}};
        }

        [[nodiscard]]
        nlohmann::json build_responses_image_input(const ToolResult &result) {
            nlohmann::json content = nlohmann::json::array();
            for (const auto &image : result.images) {
                content.push_back({
                    {"type", "input_image"},
                    {"image_url", "data:" + image.media_type + ";base64," + image.data},
                });
            }
            return {{"role", "user"}, {"content", content}};
        }

        void append_serialized_message(nlohmann::json &chat_messages, const Message &message) {
            if (auto converted = serialize_message(message); converted.has_value()) {
                chat_messages.push_back(std::move(*converted));
            }
        }

        void append_user_message(nlohmann::json &chat_messages, const Message &message) {
            bool saw_tool_results = false;
            std::string user_text;

            for (const auto &block : message) {
                if (const auto *result = std::get_if<ToolResult>(&block)) {
                    saw_tool_results = true;
                    if (result->tool_use_id.empty()) {
                        spdlog::warn("skipping tool result with empty tool call id while serializing history");
                        continue;
                    }
                    chat_messages.push_back({{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}});
                    if (!result->images.empty()) {
                        chat_messages.push_back(build_image_user_message(*result));
                    }
                } else if (const auto *text = std::get_if<Text>(&block)) {
                    user_text += text->text;
                }
            }

            if (saw_tool_results) {
                if (!user_text.empty()) {
                    chat_messages.push_back({{"role", "user"}, {"content", user_text}});
                }
                return;
            }

            append_serialized_message(chat_messages, message);
        }

    } // namespace

    void merge_tool_call_delta(ToolCallState &state, const nlohmann::json &delta) {
        if (delta.contains("id") && delta["id"].is_string()) {
            const auto id = delta["id"].get<std::string>();
            if (!id.empty()) {
                state.id = id;
            }
        }

        if (!delta.contains("function") || !delta["function"].is_object()) {
            return;
        }

        const auto &function = delta["function"];
        if (function.contains("name") && function["name"].is_string()) {
            merge_tool_name(state.name, function["name"].get<std::string>());
        }
        if (function.contains("arguments") && function["arguments"].is_string()) {
            state.arguments += function["arguments"].get<std::string>();
        }
    }

    std::optional<ToolUse> finalize_tool_call(const ToolCallState &state, std::string_view source) {
        if (state.id.empty() || state.name.empty()) {
            spdlog::warn("dropping incomplete {} tool call", source);
            return std::nullopt;
        }

        nlohmann::json input = nlohmann::json::object();
        if (!state.arguments.empty()) {
            try {
                input = nlohmann::json::parse(state.arguments);
            } catch (const nlohmann::json::parse_error &) {
                spdlog::warn("failed to parse {} tool arguments for '{}'", source, state.name);
            }
        }

        return ToolUse(state.id, state.name, std::move(input));
    }

    std::optional<nlohmann::json> serialize_assistant_message(const Message &message) {
        nlohmann::json payload;
        payload["role"] = "assistant";

        std::string text_content;
        nlohmann::json tool_calls = nlohmann::json::array();
        for (const auto &block : message) {
            if (std::get_if<Thinking>(&block) != nullptr) {
                continue;
            }
            if (const auto *text = std::get_if<Text>(&block)) {
                text_content += text->text;
                continue;
            }

            const auto *tool = std::get_if<ToolUse>(&block);
            if (tool == nullptr) {
                continue;
            }
            if (!valid_tool_use(*tool)) {
                spdlog::warn("skipping malformed assistant tool call while serializing history");
                continue;
            }

            tool_calls.push_back({{"id", tool->id}, {"type", "function"}, {"function", {{"name", tool->name}, {"arguments", tool->input.dump()}}}});
        }

        if (text_content.empty() && tool_calls.empty()) {
            return std::nullopt;
        }

        payload["content"] = text_content.empty() ? nlohmann::json(nullptr) : nlohmann::json(text_content);
        if (!tool_calls.empty()) {
            payload["tool_calls"] = std::move(tool_calls);
        }
        return payload;
    }

    std::optional<nlohmann::json> serialize_message(const Message &message) {
        if (message.role() == base::role::assistant) {
            return serialize_assistant_message(message);
        }

        if (message.role() == base::role::user) {
            for (const auto &block : message) {
                if (const auto *result = std::get_if<ToolResult>(&block)) {
                    if (result->tool_use_id.empty()) {
                        spdlog::warn("skipping tool result with empty tool call id while serializing history");
                        continue;
                    }
                    return nlohmann::json{{"role", "tool"}, {"tool_call_id", result->tool_use_id}, {"content", result->content}};
                }
            }

            std::string text;
            for (const auto &block : message) {
                if (const auto *text_block = std::get_if<Text>(&block)) {
                    text += text_block->text;
                }
            }
            if (text.empty()) {
                return std::nullopt;
            }
            return nlohmann::json{{"role", "user"}, {"content", text}};
        }

        return nlohmann::json{{"role", magic_enum::enum_name(message.role())}, {"content", ""}};
    }

    void append_chat_history_message(nlohmann::json &chat_messages, const Message &message) {
        if (message.role() == base::role::user) {
            append_user_message(chat_messages, message);
            return;
        }
        append_serialized_message(chat_messages, message);
    }

    void append_responses_history_message(nlohmann::json &responses_input, const Message &message) {
        auto converted = serialize_message(message);
        if (!converted.has_value()) {
            return;
        }

        if (converted->contains("tool_call_id")) {
            responses_input.push_back({
                {"type", "function_call_output"},
                {"call_id", (*converted)["tool_call_id"]},
                {"output", converted->value("content", std::string{})},
            });
            for (const auto &block : message) {
                if (const auto *result = std::get_if<ToolResult>(&block); result != nullptr && !result->images.empty()) {
                    responses_input.push_back(build_responses_image_input(*result));
                }
            }
            return;
        }

        const auto role = converted->value("role", "user");
        if (converted->contains("content") && (*converted)["content"].is_string()) {
            const auto content = (*converted)["content"].get<std::string>();
            if (!content.empty()) {
                responses_input.push_back({{"role", role}, {"content", content}});
            }
        }

        if (converted->contains("tool_calls")) {
            for (const auto &tool_call : (*converted)["tool_calls"]) {
                responses_input.push_back({
                    {"type", "function_call"},
                    {"call_id", tool_call["id"]},
                    {"name", tool_call["function"]["name"]},
                    {"arguments", tool_call["function"]["arguments"]},
                });
            }
        }
    }

    nlohmann::json chat_tool_to_json(const ToolDef &tool) {
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

} // namespace orangutan::providers::protocols::openai
