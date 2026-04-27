#include "bootstrap/memory-context.hpp"

namespace orangutan::bootstrap {

    RuntimeMemoryContext make_runtime_memory_context(const RuntimeIdentity &identity) {
        return {
            .scope = identity.memory_scope,
            .workspace = identity.workspace,
        };
    }

} // namespace orangutan::bootstrap
