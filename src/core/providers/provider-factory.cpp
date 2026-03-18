#include "core/providers/provider.hpp"

#include "core/providers/anthropic-provider.hpp"
#include "core/providers/openai-provider.hpp"

#include <algorithm>
#include <mutex>
#include <sstream>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <utility>

namespace orangutan {
namespace {

class NonRetryableProviderError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::unique_ptr<Provider> instantiate_provider(const ProviderEndpoint &endpoint) {
    if (endpoint.provider_name == "anthropic" || endpoint.provider_name.empty()) {
        const auto url = endpoint.base_url.empty() ? std::string("https://api.anthropic.com") : endpoint.base_url;
        return std::make_unique<AnthropicProvider>(endpoint.api_key, endpoint.model, url);
    }

    if (endpoint.provider_name == "openai") {
        const auto url = endpoint.base_url.empty() ? std::string("https://api.openai.com") : endpoint.base_url;
        return std::make_unique<OpenAiProvider>(endpoint.api_key, endpoint.model, url);
    }

    throw std::runtime_error("Unknown provider: " + endpoint.provider_name + ". Supported: anthropic, openai");
}

class FallbackProvider final : public Provider {
public:
    FallbackProvider(std::vector<ProviderEndpoint> endpoints, ProviderFactory factory)
    : endpoints_(std::move(endpoints)),
      factory_(std::move(factory)) {
        if (endpoints_.empty()) {
            throw std::runtime_error("FallbackProvider requires at least one model endpoint");
        }
        if (!factory_) {
            factory_ = instantiate_provider;
        }
    }

    LLMResponse chat(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int max_tokens) override {
        return execute_with_fallback([&](Provider &provider) {
            return provider.chat(system_prompt, messages, tools, max_tokens);
        });
    }

    LLMResponse chat_stream(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, const StreamCallback &on_event,
                            int max_tokens) override {
        return execute_with_fallback([&](Provider &provider) {
            bool emitted_output = false;
            auto tracking_callback = [&on_event, &emitted_output](const std::string &event_type, const json &data) {
                emitted_output = true;
                if (on_event) {
                    on_event(event_type, data);
                }
            };

            try {
                return provider.chat_stream(system_prompt, messages, tools, tracking_callback, max_tokens);
            } catch (const std::exception &error) {
                if (emitted_output) {
                    throw NonRetryableProviderError(error.what());
                }
                throw;
            }
        });
    }

    [[nodiscard]]
    std::string name() const override {
        std::scoped_lock lock(mutex_);
        return endpoints_[active_index_].provider_name.empty() ? std::string("anthropic") : endpoints_[active_index_].provider_name;
    }

    [[nodiscard]]
    std::string current_model() const override {
        std::scoped_lock lock(mutex_);
        return endpoints_[active_index_].model;
    }

    [[nodiscard]]
    ProviderUsageStats usage() const override {
        std::scoped_lock lock(mutex_);
        return usage_;
    }

private:
    std::vector<ProviderEndpoint> endpoints_;
    ProviderFactory factory_;
    mutable std::mutex mutex_;
    size_t active_index_ = 0;
    std::shared_ptr<Provider> active_provider_;
    ProviderUsageStats usage_;

    std::shared_ptr<Provider> active_provider_locked() {
        if (!active_provider_) {
            active_provider_ = std::move(factory_(endpoints_[active_index_]));
        }
        return active_provider_;
    }

    template <typename Fn>
    LLMResponse execute_with_fallback(Fn &&fn) {
        std::vector<std::string> errors;
        {
            std::scoped_lock lock(mutex_);
            ++usage_.logical_requests;
        }

        while (true) {
            try {
                std::unique_lock lock(mutex_);
                ++usage_.attempt_count;
                auto provider = active_provider_locked();
                lock.unlock();
                return fn(*provider);
            } catch (const NonRetryableProviderError &) {
                std::scoped_lock lock(mutex_);
                ++usage_.failed_attempts;
                throw;
            } catch (const std::exception &error) {
                std::scoped_lock lock(mutex_);
                ++usage_.failed_attempts;

                std::ostringstream message;
                message << endpoints_[active_index_].model << ": " << error.what();
                errors.push_back(message.str());

                if (active_index_ + 1 >= endpoints_.size()) {
                    std::ostringstream summary;
                    summary << "All configured models failed";
                    if (!errors.empty()) {
                        summary << " (" << errors.front();
                        for (size_t index = 1; index < errors.size(); ++index) {
                            summary << "; " << errors[index];
                        }
                        summary << ")";
                    }
                    throw std::runtime_error(summary.str());
                }

                const auto previous_model = endpoints_[active_index_].model;
                ++active_index_;
                active_provider_.reset();
                ++usage_.fallback_switches;
                spdlog::warn("Model '{}' failed, falling back to '{}': {}", previous_model, endpoints_[active_index_].model, error.what());
            }
        }
    }
};

std::vector<ProviderEndpoint> build_provider_chain(const std::string &provider_name, const std::string &api_key, const std::string &model, const std::string &base_url,
                                                   const std::vector<std::string> &fallback_models) {
    std::vector<ProviderEndpoint> endpoints;
    endpoints.push_back({
        .provider_name = provider_name,
        .api_key = api_key,
        .model = model,
        .base_url = base_url,
    });

    for (const auto &fallback_model : fallback_models) {
        if (fallback_model.empty() || fallback_model == model) {
            continue;
        }
        if (std::ranges::any_of(endpoints, [&](const ProviderEndpoint &endpoint) {
                return endpoint.model == fallback_model;
            })) {
            continue;
        }
        endpoints.push_back({
            .provider_name = provider_name,
            .api_key = api_key,
            .model = fallback_model,
            .base_url = base_url,
        });
    }

    return endpoints;
}

} // namespace

std::unique_ptr<Provider> create_provider(const std::string &provider_name, const std::string &api_key, const std::string &model, const std::string &base_url) {
    return create_provider_with_fallbacks(provider_name, api_key, model, base_url, {});
}

std::unique_ptr<Provider> create_provider_with_fallbacks(const std::string &provider_name, const std::string &api_key, const std::string &model,
                                                         const std::string &base_url, const std::vector<std::string> &fallback_models, ProviderFactory factory) {
    return std::make_unique<FallbackProvider>(build_provider_chain(provider_name, api_key, model, base_url, fallback_models), std::move(factory));
}

} // namespace orangutan
