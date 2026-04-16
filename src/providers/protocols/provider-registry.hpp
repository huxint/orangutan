#pragma once

#include "providers/protocols/protocol-adapter.hpp"

#include <vector>

namespace orangutan::providers::protocols {

    /// Describes one provider/protocol pair: the adapter that serializes
    /// requests and the auth function that applies credentials to headers.
    /// Adding a new vendor is a matter of pushing another descriptor.
    struct ProviderDescriptor {
        provider_kind provider;
        protocol_kind protocol;
        std::shared_ptr<const ProtocolAdapter> adapter;
        AuthFn auth;
    };

    class ProviderRegistry {
    public:
        ProviderRegistry();

        ProviderRegistry &register_descriptor(ProviderDescriptor descriptor) &;

        [[nodiscard]]
        ProviderAssembly resolve(provider_kind provider, protocol_kind protocol) const;

    private:
        std::vector<ProviderDescriptor> descriptors_;
    };

} // namespace orangutan::providers::protocols
