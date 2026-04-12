#pragma once

#include "skills/runtime.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace orangutan::skills {

    std::vector<std::filesystem::path> resolve_skill_directories(const std::vector<std::string> &configured_skill_paths, const std::filesystem::path &workspace_root);

    class SkillLoader {
    public:
        void load_from_directories(const std::vector<std::filesystem::path> &directories);

        void set_source(skill_source source) {
            source_ = source;
        }

        void set_workspace_root(const std::filesystem::path &workspace_root) {
            workspace_root_ = workspace_root;
        }

        void activate_for_paths(const std::vector<std::filesystem::path> &paths);

        [[nodiscard]]
        skill_invoke_result invoke(const skill_invoke_request &request) const;

        [[nodiscard]]
        skill_catalog_view list(const skill_list_query &query) const;

        [[nodiscard]]
        const std::vector<skill_diagnostic> &diagnostics() const;

    private:
        skill_runtime runtime_;
        std::filesystem::path workspace_root_;
        skill_source source_ = skill_source::workspace;
    };

} // namespace orangutan::skills

namespace orangutan {

    using skills::resolve_skill_directories;
    using skills::SkillLoader;

} // namespace orangutan
