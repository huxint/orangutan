#include "tools/skill/skill-tool.hpp"

#include "skills/skill-loader.hpp"
#include "utils/format.hpp"

namespace orangutan::tools {

    namespace {

        std::string execute_skill_tool(const nlohmann::json &input, const skills::SkillLoader &loader) {
            const auto name = input.value("name", "");
            if (name.empty()) {
                return "Error: skill name is required.";
            }

            const auto *skill = loader.find_skill(name);
            if (skill == nullptr) {
                std::string available;
                for (const auto &s : loader.active_skills()) {
                    if (!available.empty()) {
                        available += ", ";
                    }
                    available += s.name;
                }
                return "Error: skill '" + name + "' not found. Available skills: " + available;
            }

            std::string out;
            utils::format_to(out, "# Skill: {}\n\n{}", skill->name, skill->body);
            return out;
        }

    } // namespace

    void register_skill_tool(ToolRegistry &registry, const skills::SkillLoader &skill_loader) {
        if (skill_loader.active_skills().empty()) {
            return;
        }

        registry.register_tool({
            .definition =
                {
                    .name = "skill",
                    .description = "Load the full instructions for a skill by name. Use this before following a skill's guidance.",
                    .input_schema =
                        {
                            {"type", "object"},
                            {"properties",
                             {
                                 {"name", {{"type", "string"}, {"description", "The name of the skill to load"}}},
                             }},
                            {"required", nlohmann::json::array({"name"})},
                        },
                },
            .execute =
                [&skill_loader](const nlohmann::json &input) {
                    return execute_skill_tool(input, skill_loader);
                },
        });
    }

} // namespace orangutan::tools
