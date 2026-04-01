#pragma once

#include "tools/registry/tool.hpp"

namespace orangutan::memory {
    class RuntimeMemory;
}

namespace orangutan::tools::memory {

    void register_tools(ToolRegistry &registry, orangutan::memory::RuntimeMemory &runtime_memory);

} // namespace orangutan::tools::memory
