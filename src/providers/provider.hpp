#pragma once

#include "types/types.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::providers {

    // Callback for streaming events: (event_type, json_data)
    using StreamCallback = std::function<void(const std::string &event_type, const nlohmann::json &data)>;

    struct ProviderUsageStats {
        std::size_t logical_requests = 0;
        std::size_t attempt_count = 0;
        std::size_t failed_attempts = 0;
        std::size_t fallback_switches = 0;
    };

    struct ProviderEndpoint {
        std::string profile_name;
        std::string endpoint_style;
        std::string api_key;
        std::string model;
        std::string base_url;
        std::unordered_map<std::string, std::string> headers;
        std::optional<int> default_max_tokens;
        std::string thinking = "none";
    };

    using ProviderFactory = std::function<std::unique_ptr<class Provider>(const ProviderEndpoint &)>;

    class MissingApiKeyError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // Abstract provider interface — all LLM backends implement this
    class Provider {
    public:
        virtual ~Provider() = default;

        Provider(const Provider &) = delete;
        Provider &operator=(const Provider &) = delete;
        Provider(Provider &&) = delete;
        Provider &operator=(Provider &&) = delete;

        // Send messages and get full response (blocking)
        virtual LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens = 4096,
                                 int thinking_budget = 0) = 0;

        // Send messages with streaming
        virtual LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                                        int max_tokens = 4096, int thinking_budget = 0) = 0;

        // Provider name for logging
        [[nodiscard]]
        virtual std::string name() const = 0;

        [[nodiscard]]
        virtual std::string current_model() const {
            return {};
        }

        [[nodiscard]]
        virtual ProviderUsageStats usage() const {
            return {};
        }

    protected:
        Provider() = default;
    };

    std::unique_ptr<Provider> create_provider(const ProviderEndpoint &endpoint, ProviderFactory factory = {});

    std::unique_ptr<Provider> create_provider_with_fallbacks(const ProviderEndpoint &primary_endpoint, std::vector<ProviderEndpoint> fallback_endpoints = {},
                                                             ProviderFactory factory = {});

} // namespace orangutan::providers

namespace orangutan {

    using providers::create_provider;
    using providers::create_provider_with_fallbacks;
    using providers::MissingApiKeyError;
    using providers::Provider;
    using providers::ProviderEndpoint;
    using providers::ProviderFactory;
    using providers::ProviderUsageStats;
    using providers::StreamCallback;

} // namespace orangutan
