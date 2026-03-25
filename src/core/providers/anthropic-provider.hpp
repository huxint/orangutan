#pragma once

#include "core/providers/provider.hpp"
#include "core/providers/sse-parser.hpp"

#include <string>
#include <vector>

namespace orangutan {

class AnthropicProvider : public Provider {
public:
    AnthropicProvider(std::string api_key, std::string model, std::string base_url = "https://api.anthropic.com");

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
        return model_;
    }

private:
    std::string api_key_;
    std::string model_;
    std::string base_url_;

    [[nodiscard]]
    json build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, bool stream,
                            int thinking_budget = 0) const;

    [[nodiscard]]
    static LLMResponse parse_response(const json &response_json);
};

} // namespace orangutan
