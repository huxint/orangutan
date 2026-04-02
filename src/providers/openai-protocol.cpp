#include "providers/openai-protocol.hpp"

#include <spdlog/spdlog.h>

namespace orangutan::providers::detail {
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

    } // namespace

    void merge_chat_completions_tool_call_delta(OpenAiToolCallState &state, const nlohmann::json &delta) {
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

    std::optional<ToolUse> finalize_openai_tool_call(const OpenAiToolCallState &state, std::string_view source) {
        if (state.id.empty() || state.name.empty()) {
            spdlog::warn("Dropping incomplete {} tool call (id='{}', name='{}')", source, state.id, state.name);
            return std::nullopt;
        }

        nlohmann::json input = nlohmann::json::object();
        if (!state.arguments.empty()) {
            try {
                input = nlohmann::json::parse(state.arguments);
            } catch (const nlohmann::json::parse_error &) {
                spdlog::warn("Failed to parse {} tool call arguments for '{}'", source, state.name);
            }
        }

        return ToolUse(state.id, state.name, std::move(input));
    }

    bool is_valid_openai_tool_use(const ToolUse &tool_use) {
        return !tool_use.id.empty() && !tool_use.name.empty();
    }

    std::optional<nlohmann::json> serialize_openai_assistant_message(const Message &message) {
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

            if (!is_valid_openai_tool_use(*tool)) {
                spdlog::warn("Skipping malformed assistant tool call while serializing history (id='{}', name='{}')", tool->id, tool->name);
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

} // namespace orangutan::providers::detail
