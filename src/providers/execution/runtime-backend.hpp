#pragma once

#include "providers/provider.hpp"

#include <memory>

namespace orangutan::providers::execution {

    [[nodiscard]]
    std::shared_ptr<ProviderBackend> make_runtime_backend();

} // namespace orangutan::providers::execution
