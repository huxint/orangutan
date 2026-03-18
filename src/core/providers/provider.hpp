#pragma once

#include "core/types.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orangutan {

// Callback for streaming events: (event_type, json_data)
using StreamCallback = std::function<void(const std::string &event_type, const json &data)>;

// Abstract provider interface — all LLM backends implement this
class Provider {
public:
    virtual ~Provider() = default;

    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    Provider(Provider &&) = delete;
    Provider &operator=(Provider &&) = delete;

    // Send messages and get full response (blocking)
    virtual LLMResponse chat(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens = 4096) = 0;

    // Send messages with streaming
    virtual LLMResponse chat_stream(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                    int max_tokens = 4096) = 0;

    // Provider name for logging
    [[nodiscard]]
    virtual std::string name() const = 0;

protected:
    Provider() = default;
};

// Factory: create a provider by name
std::unique_ptr<Provider> create_provider(const std::string &provider_name, const std::string &api_key, const std::string &model, const std::string &base_url);

} // namespace orangutan
