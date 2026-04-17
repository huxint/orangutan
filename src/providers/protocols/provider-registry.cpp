#include "providers/protocols/provider-registry.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "providers/protocols/anthropic-messages.hpp"
#include "providers/protocols/openai-chat-completions.hpp"
#include "providers/protocols/openai-responses.hpp"
#include "utils/enum-string.hpp"

namespace orangutan::providers::protocols {
    namespace {

        void openai_auth(const ModelTarget &target, transport::header_map &headers) {
            headers.try_emplace("Content-Type", "application/json");
            headers.try_emplace("Authorization", "Bearer " + target.api_key);
        }

        void anthropic_auth(const ModelTarget &target, transport::header_map &headers) {
            headers.try_emplace("Content-Type", "application/json");
            headers.try_emplace("x-api-key", target.api_key);
            headers.try_emplace("anthropic-version", "2023-06-01");
        }

    } // namespace

    ProviderRegistry::ProviderRegistry() {
        register_descriptor({
            .provider = provider_kind::openai,
            .protocol = protocol_kind::chat_completions,
            .adapter = make_openai_chat_completions_adapter(),
            .auth = openai_auth,
        });
        register_descriptor({
            .provider = provider_kind::openai,
            .protocol = protocol_kind::responses,
            .adapter = make_openai_responses_adapter(),
            .auth = openai_auth,
        });
        register_descriptor({
            .provider = provider_kind::anthropic,
            .protocol = protocol_kind::messages,
            .adapter = make_anthropic_messages_adapter(),
            .auth = anthropic_auth,
        });
    }

    ProviderRegistry &ProviderRegistry::register_descriptor(ProviderDescriptor descriptor) & {
        descriptors_.push_back(std::move(descriptor));
        return *this;
    }

    ProviderAssembly ProviderRegistry::resolve(provider_kind provider, protocol_kind protocol) const {
        const auto it = std::ranges::find_if(descriptors_, [provider, protocol](const ProviderDescriptor &descriptor) {
            return descriptor.provider == provider && descriptor.protocol == protocol;
        });
        if (it == descriptors_.end()) {
            throw ProviderError(error_category::configuration,
                                "unsupported provider/protocol combination: " + std::string(utils::enum_name(provider)) + " + " +
                                    std::string(utils::enum_name_kebab(protocol)));
        }

        return ProviderAssembly{.adapter = it->adapter, .auth = it->auth};
    }

} // namespace orangutan::providers::protocols
