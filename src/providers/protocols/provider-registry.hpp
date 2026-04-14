#pragma once

#include "providers/protocols/protocol-adapter.hpp"

namespace orangutan::providers::protocols {

    class ProviderRegistry {
    public:
        [[nodiscard]]
        ProviderAssembly resolve(provider_kind provider, protocol_kind protocol) const;
    };

} // namespace orangutan::providers::protocols
