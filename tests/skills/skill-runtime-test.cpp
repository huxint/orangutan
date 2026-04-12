#include "skills/runtime.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace orangutan::skills;

namespace {

    void write_skill(const std::filesystem::path &root, const std::string &dir_name, const std::string &frontmatter, const std::string &body) {
        const auto skill_dir = root / dir_name;
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "---\n";
        out << frontmatter;
        out << "\n---\n\n";
        out << body;
        out << "\n";
    }

    TEST_CASE("runtime_reload_builds_catalog_and_stable_ids") {
        const auto root = orangutan::testing::unique_test_root("skill-runtime-id");
        write_skill(root, "alpha", "name: alpha\ndescription: alpha skill", "body");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {root},
            .workspace_root = root,
        });

        const auto catalog = runtime.list({});
        REQUIRE(catalog.skills.size() == 1UL);
        CHECK(catalog.skills[0].name == "alpha");
        CHECK(catalog.skills[0].id.contains("#alpha"));

        std::filesystem::remove_all(root);
    }

    TEST_CASE("runtime_uses_directory_precedence_for_same_name") {
        const auto low_root = orangutan::testing::unique_test_root("skill-runtime-low");
        const auto high_root = orangutan::testing::unique_test_root("skill-runtime-high");
        write_skill(low_root, "same", "name: same\ndescription: low", "low body");
        write_skill(high_root, "same", "name: same\ndescription: high", "high body");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {low_root, high_root},
            .workspace_root = low_root,
        });

        const auto *skill = runtime.find_by_name("same");
        REQUIRE(skill != nullptr);
        CHECK(skill->description == "high");

        std::filesystem::remove_all(low_root);
        std::filesystem::remove_all(high_root);
    }

    TEST_CASE("runtime_conditional_skill_activation_and_blocking") {
        const auto workspace = orangutan::testing::unique_test_root("skill-runtime-conditional");
        write_skill(workspace, "conditional", "name: conditional\ndescription: conditional skill\nscope: conditional\npaths_any: [src/*.cpp]", "hello");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {workspace},
            .workspace_root = workspace,
        });

        {
            const auto blocked = runtime.invoke({
                .name = "conditional",
                .call_origin = skill_call_origin::automatic,
            });
            CHECK(blocked.status == skill_invoke_status::blocked);
        }

        runtime.activate_for_paths({workspace / "src" / "main.cpp"});

        {
            const auto ok = runtime.invoke({
                .name = "conditional",
                .call_origin = skill_call_origin::automatic,
            });
            CHECK(ok.status == skill_invoke_status::ok);
            CHECK(ok.content.contains("hello"));
        }

        std::filesystem::remove_all(workspace);
    }

    TEST_CASE("runtime_blocks_manual_only_for_automatic_origin") {
        const auto root = orangutan::testing::unique_test_root("skill-runtime-manual-only");
        write_skill(root, "manual", "name: manual\ndescription: manual only skill\nscope: manual_only", "manual body");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {root},
            .workspace_root = root,
        });

        const auto blocked = runtime.invoke({
            .name = "manual",
            .call_origin = skill_call_origin::automatic,
        });
        CHECK(blocked.status == skill_invoke_status::blocked);

        const auto allowed = runtime.invoke({
            .name = "manual",
            .call_origin = skill_call_origin::manual,
        });
        CHECK(allowed.status == skill_invoke_status::ok);
        CHECK(allowed.content.contains("manual body"));

        std::filesystem::remove_all(root);
    }

    TEST_CASE("runtime_probe_reload_lists_new_skills_without_manual_reload") {
        const auto root = orangutan::testing::unique_test_root("skill-runtime-probe-list");
        write_skill(root, "alpha", "name: alpha\ndescription: alpha skill", "alpha body");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {root},
            .workspace_root = root,
        });

        const auto before = runtime.list({});
        REQUIRE(before.skills.size() == 1UL);
        CHECK(before.skills[0].name == "alpha");

        write_skill(root, "beta", "name: beta\ndescription: beta skill", "beta body");

        const auto after = runtime.list({});
        CHECK(after.skills.size() == 2UL);
        CHECK(std::ranges::any_of(after.skills, [](const skill_view &skill) {
            return skill.name == "beta";
        }));

        std::filesystem::remove_all(root);
    }

    TEST_CASE("runtime_probe_reload_allows_invoking_new_skill_without_manual_reload") {
        const auto root = orangutan::testing::unique_test_root("skill-runtime-probe-invoke");
        write_skill(root, "alpha", "name: alpha\ndescription: alpha skill", "alpha body");

        skill_runtime runtime;
        runtime.reload(skill_runtime_config{
            .directories = {root},
            .workspace_root = root,
        });

        const auto missing_before = runtime.invoke({
            .name = "beta",
            .call_origin = skill_call_origin::automatic,
        });
        CHECK(missing_before.status == skill_invoke_status::not_found);

        write_skill(root, "beta", "name: beta\ndescription: beta skill", "beta body updated for probe signature");

        const auto loaded_after = runtime.invoke({
            .name = "beta",
            .call_origin = skill_call_origin::automatic,
        });
        CHECK(loaded_after.status == skill_invoke_status::ok);
        CHECK(loaded_after.content.contains("beta body updated for probe signature"));

        std::filesystem::remove_all(root);
    }

} // namespace
