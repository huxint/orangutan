#pragma once

#include "bootstrap/identity.hpp"

#include <string>

namespace orangutan::bootstrap {

    struct RuntimeMemoryContext {
        std::string scope;
        std::string workspace;
    };

    [[nodiscard]]
    RuntimeMemoryContext make_runtime_memory_context(const RuntimeIdentity &identity);

} // namespace orangutan::bootstrap
