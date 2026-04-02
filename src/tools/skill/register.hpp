#pragma once

#include "tools/registry/tool-registry.hpp"

namespace orangutan::skills {
    class SkillLoader;
}

namespace orangutan::tools::skill {

    void register_tools(ToolRegistry &registry, const skills::SkillLoader &skill_loader);

} // namespace orangutan::tools::skill
