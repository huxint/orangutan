#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::skills {

    std::vector<std::filesystem::path> resolve_skill_directories(const std::vector<std::string> &configured_skill_paths, const std::filesystem::path &workspace_root);

    struct SkillDef {
        std::string name;
        std::string description;
        std::vector<std::string> tools;
        std::vector<std::string> env;
        std::string body;
        std::string source_path;
    };

    class SkillLoader {
    public:
        void load_from_directories(const std::vector<std::filesystem::path> &directories);

        [[nodiscard]]
        const std::vector<SkillDef> &active_skills() const {
            return skills_;
        }

        [[nodiscard]]
        const SkillDef *find_skill(std::string_view name) const;

        [[nodiscard]]
        std::string build_prompt_section() const;

    private:
        std::vector<SkillDef> skills_;
    };

} // namespace orangutan::skills

namespace orangutan {

    using skills::resolve_skill_directories;
    using skills::SkillDef;
    using skills::SkillLoader;

} // namespace orangutan
