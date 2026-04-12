#include "tools/skill/skill-tool.hpp"

#include "skills/skill-loader.hpp"

namespace orangutan::tools {

    namespace {

        std::string execute_skill_tool(const nlohmann::json &input, const skills::SkillLoader &loader) {
            const auto name = input.value("name", "");
            if (name.empty()) {
                return "Error: skill name is required.";
            }

            const auto result = loader.invoke(skills::skill_invoke_request{
                .name = name,
                .call_origin = skills::skill_call_origin::automatic,
            });

            if (result.status == skills::skill_invoke_status::ok) {
                return result.content;
            }

            if (result.status == skills::skill_invoke_status::not_found) {
                std::string available;
                const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
                for (const auto &s : catalog.skills) {
                    if (!available.empty()) {
                        available += ", ";
                    }
                    available += s.name;
                }
                return "Error: skill '" + name + "' not found. Available skills: " + available;
            }

            if (!result.diagnostics.empty()) {
                return "Error: failed to load skill '" + name + "': " + result.diagnostics.front().message;
            }
            return "Error: failed to load skill '" + name + "'.";
        }

    } // namespace

    void register_skill_tool(ToolRegistry &registry, const skills::SkillLoader &skill_loader) {
        const auto catalog = skill_loader.list(skills::skill_list_query{.include_inactive = true});
        if (catalog.skills.empty()) {
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
