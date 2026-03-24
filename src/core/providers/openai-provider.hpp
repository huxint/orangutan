#pragma once

#include "core/providers/provider.hpp"
#include <string>
#include <vector>

namespace orangutan {

// SseParser is defined in anthropic-provider.hpp and shared across providers
class SseParser;

class OpenAiProvider : public Provider {
public:
    OpenAiProvider(std::string api_key, std::string model, std::string base_url = "https://api.openai.com");

    LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens = 4096) override;

    LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                            int max_tokens = 4096) override;

    [[nodiscard]]
    std::string name() const override {
        return "openai";
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
    json build_request_body(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens, bool stream) const;

    // Convert orangutan Message to OpenAI message format
    [[nodiscard]]
    static json message_to_openai(const Message &msg);

    // Parse OpenAI response into LLMResponse
    [[nodiscard]]
    static LLMResponse parse_response(const json &response_json);
};

} // namespace orangutan
