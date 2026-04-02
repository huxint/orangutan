#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::tools {

    void register_skill_tool(ToolRegistry &registry, const skills::SkillLoader &skill_loader);

} // namespace orangutan::tools
