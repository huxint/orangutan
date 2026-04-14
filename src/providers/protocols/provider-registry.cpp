#include "providers/protocols/provider-registry.hpp"

#include <memory>

#include "providers/protocols/anthropic-messages.hpp"
#include "providers/protocols/openai-chat-completions.hpp"
#include "providers/protocols/openai-responses.hpp"

namespace orangutan::providers::protocols {
    namespace {

        class OpenAiAuthStrategy final : public AuthStrategy {
        public:
            void apply(const ModelTarget &target, transport::header_map &headers) const override {
                if (!headers.contains("Content-Type")) {
                    headers.emplace("Content-Type", "application/json");
                }
                if (!headers.contains("Authorization")) {
                    headers.emplace("Authorization", "Bearer " + target.api_key);
                }
            }
        };

        class AnthropicAuthStrategy final : public AuthStrategy {
        public:
            void apply(const ModelTarget &target, transport::header_map &headers) const override {
                if (!headers.contains("Content-Type")) {
                    headers.emplace("Content-Type", "application/json");
                }
                if (!headers.contains("x-api-key")) {
                    headers.emplace("x-api-key", target.api_key);
                }
                if (!headers.contains("anthropic-version")) {
                    headers.emplace("anthropic-version", "2023-06-01");
                }
            }
        };

    } // namespace

    ProviderAssembly ProviderRegistry::resolve(provider_kind provider, protocol_kind protocol) const {
        static const auto openai_auth = std::make_shared<OpenAiAuthStrategy>();
        static const auto anthropic_auth = std::make_shared<AnthropicAuthStrategy>();
        static const auto openai_chat = make_openai_chat_completions_adapter();
        static const auto openai_responses = make_openai_responses_adapter();
        static const auto anthropic_messages = make_anthropic_messages_adapter();

        if (provider == provider_kind::openai && protocol == protocol_kind::chat_completions) {
            return ProviderAssembly{.adapter = openai_chat, .auth = openai_auth};
        }
        if (provider == provider_kind::openai && protocol == protocol_kind::responses) {
            return ProviderAssembly{.adapter = openai_responses, .auth = openai_auth};
        }
        if (provider == provider_kind::anthropic && protocol == protocol_kind::messages) {
            return ProviderAssembly{.adapter = anthropic_messages, .auth = anthropic_auth};
        }

        throw ProviderError(error_category::configuration,
                            "unsupported provider/protocol combination: " + std::string(to_string(provider)) + " + " + std::string(to_string(protocol)));
    }

} // namespace orangutan::providers::protocols
