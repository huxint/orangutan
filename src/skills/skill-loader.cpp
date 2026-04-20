#include "skills/skill-loader.hpp"

#include "bootstrap/identity.hpp"

#include <cstdlib>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace orangutan::skills {

    std::vector<std::filesystem::path> resolve_skill_directories(const std::vector<std::string> &configured_skill_paths, const std::filesystem::path &workspace_root) {
        if (!configured_skill_paths.empty()) {
            std::vector<std::filesystem::path> configured_paths;
            configured_paths.reserve(configured_skill_paths.size());
            for (const auto &path : configured_skill_paths) {
                configured_paths.emplace_back(path);
            }
            return configured_paths;
        }

        std::vector<std::filesystem::path> directories;
        if (const char *home = std::getenv("HOME"); home != nullptr) {
            directories.emplace_back(std::filesystem::path{home} / ".orangutan" / "skills");
        }
        if (!workspace_root.empty()) {
            directories.emplace_back(bootstrap::workspace_skills_root(workspace_root.string()));
        }
        return directories;
    }

    SkillLoaderBuilder SkillLoader::create() {
        return {};
    }

    void SkillLoader::load_from_directories(const std::vector<std::filesystem::path> &directories) {
        runtime_.reload(skill_runtime_config{
            .directories = directories,
            .workspace_root = workspace_root_,
            .source = source_,
        });

        for (const auto &diagnostic : runtime_.diagnostics()) {
            if (diagnostic.code == "missing_frontmatter") {
                spdlog::warn("Skill file has no frontmatter: {}", diagnostic.source_path);
            } else if (diagnostic.code == "unclosed_frontmatter") {
                spdlog::warn("Skill file has unclosed frontmatter: {}", diagnostic.source_path);
            } else if (diagnostic.code == "missing_name" || diagnostic.code == "missing_description") {
                spdlog::warn("Skill file missing required 'name' or 'description': {}", diagnostic.source_path);
            }
        }
    }

    void SkillLoader::activate_for_paths(const std::vector<std::filesystem::path> &paths) {
        runtime_.activate_for_paths(paths);
    }

    skill_invoke_result SkillLoader::invoke(const skill_invoke_request &request) const {
        return runtime_.invoke(request);
    }

    skill_catalog_view SkillLoader::list(const skill_list_query &query) const {
        return runtime_.list(query);
    }

    const std::vector<skill_diagnostic> &SkillLoader::diagnostics() const {
        return runtime_.diagnostics();
    }

} // namespace orangutan::skills
