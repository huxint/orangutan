#include "tools/memory/register.hpp"

#include "tools/register.hpp"

namespace orangutan::tools::memory {

    void register_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory &runtime_memory) {
        register_builtin_memory_tools(registry, runtime_memory);
    }

} // namespace orangutan::tools::memory
