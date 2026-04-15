#pragma once

#include "skills/runtime.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::skills {

    std::vector<std::filesystem::path> resolve_skill_directories(const std::vector<std::string> &configured_skill_paths, const std::filesystem::path &workspace_root);

    class SkillLoaderBuilder;

    class SkillLoader {
    public:
        void load_from_directories(const std::vector<std::filesystem::path> &directories);

        void set_source(skill_source source) {
            source_ = source;
        }

        void set_workspace_root(const std::filesystem::path &workspace_root) {
            workspace_root_ = workspace_root;
        }

        /// Create a builder for fluent SkillLoader configuration.
        [[nodiscard]]
        static SkillLoaderBuilder create();

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

    class SkillLoaderBuilder {
    public:
        auto with_source(this auto &&self, skill_source source) -> decltype(auto) {
            self.source_ = source;
            return std::forward<decltype(self)>(self);
        }

        auto with_workspace_root(this auto &&self, std::filesystem::path root) -> decltype(auto) {
            self.workspace_root_ = std::move(root);
            return std::forward<decltype(self)>(self);
        }

        auto with_directories(this auto &&self, std::vector<std::filesystem::path> dirs) -> decltype(auto) {
            self.directories_ = std::move(dirs);
            return std::forward<decltype(self)>(self);
        }

        [[nodiscard]]
        auto build() const -> std::expected<SkillLoader, std::string> {
            SkillLoader loader;
            loader.set_source(source_);
            loader.set_workspace_root(workspace_root_);

            auto directories = directories_;
            if (directories.empty()) {
                directories = resolve_skill_directories({}, workspace_root_);
            }

            loader.load_from_directories(directories);
            return loader;
        }

    private:
        skill_source source_ = skill_source::workspace;
        std::filesystem::path workspace_root_;
        std::vector<std::filesystem::path> directories_;
    };

} // namespace orangutan::skills

namespace orangutan {

    using skills::resolve_skill_directories;
    using skills::SkillLoader;

} // namespace orangutan
