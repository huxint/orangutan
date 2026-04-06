#pragma once

#include "providers/provider.hpp"
#include <optional>
#include <string>
#include <vector>

namespace orangutan::providers {

    // SseParser is defined in anthropic-provider.hpp and shared across providers
    class SseParser;

    class OpenAiProvider : public Provider {
    public:
        explicit OpenAiProvider(ProviderEndpoint endpoint);

        LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens = 4096,
                         int thinking_budget = 0) override;

        LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                int max_tokens = 4096, int thinking_budget = 0) override;

        [[nodiscard]]
        std::string name() const override {
            return "openai";
        }

        [[nodiscard]]
        std::string current_model() const override {
            return endpoint_.model;
        }

    private:
        ProviderEndpoint endpoint_;

        [[nodiscard]]
        nlohmann::json build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, bool stream,
                                          int thinking_budget) const;

        [[nodiscard]]
        nlohmann::json build_responses_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools,
                                                     int resolved_max_tokens, bool stream) const;

        [[nodiscard]]
        nlohmann::json build_chat_completions_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools,
                                                            int resolved_max_tokens, bool stream) const;

        // Convert orangutan Message to OpenAI message format
        [[nodiscard]]
        static std::optional<nlohmann::json> message_to_openai(const Message &msg);

        // Parse OpenAI response into LLMResponse
        [[nodiscard]]
        static LLMResponse parse_chat_completions_response(const nlohmann::json &response_json);
        [[nodiscard]]
        static LLMResponse parse_responses_response(const nlohmann::json &response_json);
    };

} // namespace orangutan::providers
