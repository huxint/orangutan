#include "tools/skill/register.hpp"

#include "tools/skill/skill-tool.hpp"

namespace orangutan::tools::skill {

    void register_tools(ToolRegistry &registry, const skills::SkillLoader &skill_loader) {
        register_skill_tool(registry, skill_loader);
    }

} // namespace orangutan::tools::skill
