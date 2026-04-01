#pragma once

#include "providers/provider.hpp"

#include <string>
#include <vector>

namespace orangutan::providers {

    class AnthropicProvider : public Provider {
    public:
        explicit AnthropicProvider(ProviderEndpoint endpoint);

        LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens = 4096,
                         int thinking_budget = 0) override;

        LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                int max_tokens = 4096, int thinking_budget = 0) override;

        [[nodiscard]]
        std::string name() const override {
            return "anthropic";
        }

        [[nodiscard]]
        std::string current_model() const override {
            return endpoint_.model;
        }

    private:
        ProviderEndpoint endpoint_;

        [[nodiscard]]
        nlohmann::json build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, bool stream,
                                          int thinking_budget = 0) const;

        [[nodiscard]]
        static LLMResponse parse_response(const nlohmann::json &response_json);
    };

} // namespace orangutan::providers
