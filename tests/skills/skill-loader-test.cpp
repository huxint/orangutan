#include "skills/skill-loader.hpp"
#include "test-helpers.hpp"

#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;

namespace {

    using LoadDirectoriesSignature = void (SkillLoader::*)(const std::vector<std::filesystem::path> &);
    using ResolveDirectoriesSignature = std::vector<std::filesystem::path> (*)(const std::vector<std::string> &, const std::filesystem::path &);

    template <typename Loader>
    concept has_active_skills_member = requires(const Loader &loader) { loader.active_skills(); };

    template <typename Loader>
    concept has_find_skill_member = requires(const Loader &loader) { loader.find_skill("test-skill"); };

    template <typename Loader>
    concept has_build_prompt_section_member = requires(const Loader &loader) { loader.build_prompt_section(); };

    static_assert(std::same_as<decltype(&SkillLoader::load_from_directories), LoadDirectoriesSignature>);
    static_assert(std::same_as<decltype(&resolve_skill_directories), ResolveDirectoriesSignature>);
    static_assert(!has_active_skills_member<SkillLoader>);
    static_assert(!has_find_skill_member<SkillLoader>);
    static_assert(!has_build_prompt_section_member<SkillLoader>);

    std::filesystem::path fixtures_dir() {
        return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/skills";
    }

    std::filesystem::path override_dir() {
        return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/skills-override";
    }

    using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

    void write_skill(const std::filesystem::path &root, const std::string &dir_name, const std::string &content) {
        const auto skill_dir = root / dir_name;
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << content;
    }

    std::optional<skills::skill_view> find_skill_by_name(const skills::skill_catalog_view &catalog, std::string_view name) {
        const auto it = std::ranges::find_if(catalog.skills, [name](const skills::skill_view &skill) {
            return skill.name == name;
        });
        if (it == catalog.skills.end()) {
            return std::nullopt;
        }
        return *it;
    }

    TEST_CASE("resolve_skill_directories_returns_paths") {
        const auto workspace_root = orangutan::testing::unique_test_root("skill-loader-workspace-root");
        const auto directories = resolve_skill_directories({}, workspace_root);

        REQUIRE(directories.size() >= 1UL);
        CHECK(directories.back() == workspace_root / ".orangutan" / "skills");

        std::filesystem::remove_all(workspace_root);
    };

    TEST_CASE("builder_loads_resolved_directories_from_workspace_root") {
        const auto home_root = orangutan::testing::unique_test_root("orangutan_skill_loader_builder_home");
        const auto workspace_root = orangutan::testing::unique_test_root("orangutan_skill_loader_builder_workspace");
        const auto workspace_skills = workspace_root / ".orangutan" / "skills";
        ScopedEnvVar env("HOME", home_root.string());

        write_skill(workspace_skills, "workspace-builder-skill", R"md(---
name: workspace-builder-skill
description: workspace builder skill
---
workspace builder body
)md");

        auto build_result = SkillLoader::create().with_workspace_root(workspace_root).build();
        REQUIRE(build_result.has_value());

        auto loader = std::move(build_result).value();
        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto skill = find_skill_by_name(catalog, "workspace-builder-skill");
        REQUIRE(skill.has_value());

        const auto invoke = loader.invoke({.name = "workspace-builder-skill", .call_origin = skills::skill_call_origin::manual});
        CHECK(invoke.status == skills::skill_invoke_status::ok);
        CHECK(invoke.content.contains("workspace builder body"));

        std::filesystem::remove_all(home_root);
        std::filesystem::remove_all(workspace_root);
    };

    TEST_CASE("builder_accepts_explicit_directories_without_workspace_root") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_builder_explicit");
        write_skill(temp_dir, "explicit-builder-skill", R"md(---
name: explicit-builder-skill
description: explicit builder skill
---
explicit builder body
)md");

        auto build_result = SkillLoader::create().with_directories({temp_dir}).with_source(skills::skill_source::user).build();
        REQUIRE(build_result.has_value());

        auto loader = std::move(build_result).value();
        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto skill = find_skill_by_name(catalog, "explicit-builder-skill");
        REQUIRE(skill.has_value());
        CHECK(skill->source == skills::skill_source::user);

        const auto invoke = loader.invoke({.name = "explicit-builder-skill", .call_origin = skills::skill_call_origin::manual});
        CHECK(invoke.status == skills::skill_invoke_status::ok);

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("list_excludes_unknown_skill_name") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        CHECK_FALSE(find_skill_by_name(catalog, "nonexistent-skill").has_value());
    };

    TEST_CASE("loads_valid_skill") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto skill = find_skill_by_name(catalog, "test-skill");
        INFO("expected 'test-skill' to be loaded");
        REQUIRE(skill.has_value());
        CHECK(skill->description == "A test skill for unit testing");
        CHECK(skill->tools.size() == 2UL);
        CHECK(skill->tools[0] == "read");
        CHECK(skill->tools[1] == "grep");
        CHECK(skill->body.contains("test skill body"));
    };

    TEST_CASE("skips_missing_name_field") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        for (const auto &skill : catalog.skills) {
            INFO("skill with missing name should not be loaded");
            CHECK(skill.description != "Missing the name field");
        }
    };

    TEST_CASE("skips_non_yaml_frontmatter") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        for (const auto &skill : catalog.skills) {
            INFO("skill with non-YAML frontmatter should not be loaded");
            CHECK(skill.name != "ignored-non-yaml");
        }
    };

    TEST_CASE("skips_missing_frontmatter") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_missing_frontmatter");
        write_skill(temp_dir, "missing-frontmatter", R"md(# No frontmatter here
This skill file should be skipped.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        CHECK(loader.list(skills::skill_list_query{.include_inactive = true}).skills.empty());

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("skips_unclosed_yaml_frontmatter") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_unclosed_frontmatter");
        write_skill(temp_dir, "unclosed-frontmatter", R"md(---
name: unclosed-frontmatter
description: Missing a closing delimiter
Body line that should not be parsed.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        CHECK(loader.list(skills::skill_list_query{.include_inactive = true}).skills.empty());

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("skips_unmet_env_dependency") {
        unsetenv("ORANGUTAN_TEST_ENV_SKILL");

        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        for (const auto &skill : active_catalog.skills) {
            INFO("skill with unmet env should not be active");
            CHECK(skill.name != "env-skill");
        }

        const auto all_catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto env_skill = find_skill_by_name(all_catalog, "env-skill");
        REQUIRE(env_skill.has_value());
        CHECK_FALSE(env_skill->active);
    };

    TEST_CASE("skips_missing_description_field") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_missing_description");
        write_skill(temp_dir, "missing-description", R"md(---
name: missing-description
---
This skill should be skipped.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        CHECK_FALSE(find_skill_by_name(catalog, "missing-description").has_value());

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("loads_skill_with_met_env_dependency") {
        ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "1");

        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        INFO("skill with met env should be active");
        REQUIRE(find_skill_by_name(active_catalog, "env-skill").has_value());
    };

    TEST_CASE("scoped_env_var_restores_previous_value") {
        CHECK(setenv("ORANGUTAN_TEST_ENV_SKILL", "original", 1) == 0);

        {
            ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "temporary");
            const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
            INFO("expected env var during scope");
            REQUIRE(value != nullptr);
            CHECK(std::string_view{value} == "temporary");
        }

        const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
        INFO("expected env var restored after scope");
        REQUIRE(value != nullptr);
        CHECK(std::string_view{value} == "original");
        CHECK(unsetenv("ORANGUTAN_TEST_ENV_SKILL") == 0);
    };

    TEST_CASE("later_directory_shadows_same_name") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string(), override_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto skill = find_skill_by_name(catalog, "test-skill");
        INFO("expected overridden skill to exist");
        REQUIRE(skill.has_value());
        CHECK(skill->description == "Overridden version of test-skill");
        CHECK(skill->body.contains("workspace override"));
    };

    TEST_CASE("nonexistent_directory_is_skipped") {
        SkillLoader loader;
        loader.load_from_directories({"/nonexistent/path/that/does/not/exist"});
        CHECK(loader.list(skills::skill_list_query{.include_inactive = true}).skills.empty());
    };

    TEST_CASE("render_skill_prompt_section_empty") {
        SkillLoader loader;
        loader.load_from_directories({"/nonexistent"});
        const auto section = skills::render_skill_prompt_section(loader.list(skills::skill_list_query{.include_inactive = false}));
        CHECK(section.empty());
    };

    TEST_CASE("render_skill_prompt_section_contains_skills") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto section = skills::render_skill_prompt_section(loader.list(skills::skill_list_query{.include_inactive = false}));
        CHECK(section.contains("## Available Skills"));
        CHECK(section.contains("**test-skill**"));
        CHECK(section.contains("A test skill for unit testing"));
        CHECK_FALSE(section.contains("test skill body"));
    };

    TEST_CASE("loader_preserves_configured_source_metadata") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_source_metadata");
        write_skill(temp_dir, "source-metadata", R"md(---
name: source-metadata
description: source metadata skill
---
skill body
)md");

        SkillLoader loader;
        loader.set_source(skills::skill_source::user);
        loader.load_from_directories({temp_dir});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        REQUIRE(catalog.skills.size() == 1UL);
        CHECK(catalog.skills[0].source == skills::skill_source::user);

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("loads_yaml_frontmatter_with_embedded_body_delimiter") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_embedded_delimiter");
        write_skill(temp_dir, "embedded-delimiter", R"md(---
name: embedded-delimiter
description: Contains a delimiter-like line in the body
tools: [read]
---
Body with delimiter-like content:
---
Body continues here.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        REQUIRE(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "embedded-delimiter");
        CHECK(active_catalog.skills[0].body.contains("Body with delimiter-like content"));
        CHECK(active_catalog.skills[0].body.contains("Body continues here."));

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("loads_yaml_frontmatter_with_utf8_bom_before_opening_delimiter") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_bom");
        write_skill(temp_dir, "bom-skill", std::string("\xEF\xBB\xBF") + R"md(---
name: bom-skill
description: Frontmatter starts after a UTF-8 BOM
---
Body after BOM.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        REQUIRE(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "bom-skill");

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("loads_yaml_frontmatter_with_leading_blank_line_before_opening_delimiter") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_leading_blank_line");
        write_skill(temp_dir, "blank-line-skill", R"md(
---
name: blank-line-skill
description: Frontmatter starts after a leading blank line
---
Body after leading blank line.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        REQUIRE(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "blank-line-skill");

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("continues_loading_skills_after_non_regular_skill_md_path_is_encountered") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_non_regular_skill_path");
        std::filesystem::create_directories(temp_dir / "00-non-regular-skill" / "SKILL.md");
        write_skill(temp_dir, "10-readable-skill", R"md(---
name: readable-skill
description: Should still load after a non-regular SKILL.md path
---
Readable skill body.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        REQUIRE(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "readable-skill");

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("active_catalog_is_sorted_by_name_after_shadowing") {
        const auto base_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_order_base");
        const auto override_root = orangutan::testing::unique_test_root("orangutan_skill_loader_order_override");

        write_skill(base_dir, "zebra-dir", R"md(---
name: zebra
description: zebra base
---
zebra body
)md");
        write_skill(base_dir, "alpha-dir", R"md(---
name: alpha
description: alpha base
---
alpha body
)md");
        write_skill(override_root, "alpha-override", R"md(---
name: alpha
description: alpha override
---
alpha override body
)md");

        SkillLoader loader;
        loader.load_from_directories({base_dir.string(), override_root.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        CHECK(active_catalog.skills.size() == 2UL);
        CHECK(active_catalog.skills[0].name == "alpha");
        CHECK(active_catalog.skills[0].description == "alpha override");
        CHECK(active_catalog.skills[1].name == "zebra");

        std::filesystem::remove_all(base_dir);
        std::filesystem::remove_all(override_root);
    };

    TEST_CASE("loads_yaml_frontmatter") {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto catalog = loader.list(skills::skill_list_query{.include_inactive = true});
        const auto skill = find_skill_by_name(catalog, "yaml-skill");
        INFO("expected 'yaml-skill' to be loaded");
        REQUIRE(skill.has_value());
        CHECK(skill->description == "A skill using YAML frontmatter");
        CHECK(skill->tools.size() == 2UL);
        CHECK(skill->tools[0] == "read");
        CHECK(skill->tools[1] == "write");
        CHECK(skill->body.contains("YAML skill body"));
    };

    TEST_CASE("loads_yaml_frontmatter_from_temp_dir") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_yaml");
        write_skill(temp_dir, "yaml-test", R"md(---
name: my-yaml-skill
description: Skill with YAML frontmatter
tools: [shell, grep]
env: []
---
YAML body content here.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        CHECK(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "my-yaml-skill");
        CHECK(active_catalog.skills[0].description == "Skill with YAML frontmatter");
        CHECK(active_catalog.skills[0].tools.size() == 2UL);
        CHECK(active_catalog.skills[0].tools[0] == "shell");
        CHECK(active_catalog.skills[0].tools[1] == "grep");
        CHECK(active_catalog.skills[0].body.contains("YAML body content"));

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("loads_yaml_with_quoted_values") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_yaml_quoted");
        write_skill(temp_dir, "quoted-yaml", R"md(---
name: "quoted-skill"
description: 'Single-quoted description'
tools: ["read", 'write']
---
Quoted values body.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto active_catalog = loader.list(skills::skill_list_query{.include_inactive = false});
        CHECK(active_catalog.skills.size() == 1UL);
        CHECK(active_catalog.skills[0].name == "quoted-skill");
        CHECK(active_catalog.skills[0].description == "Single-quoted description");
        CHECK(active_catalog.skills[0].tools.size() == 2UL);

        std::filesystem::remove_all(temp_dir);
    };

    TEST_CASE("render_skill_prompt_section_uses_deterministic_skill_order") {
        const auto temp_dir = orangutan::testing::unique_test_root("orangutan_skill_loader_prompt_order");

        write_skill(temp_dir, "zebra-dir", R"md(---
name: zebra
description: zebra
---
zebra prompt body
)md");
        write_skill(temp_dir, "alpha-dir", R"md(---
name: alpha
description: alpha
---
alpha prompt body
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto section = skills::render_skill_prompt_section(loader.list(skills::skill_list_query{.include_inactive = false}));
        const auto alpha_pos = section.find("**alpha**");
        const auto zebra_pos = section.find("**zebra**");
        REQUIRE(alpha_pos != std::string::npos);
        REQUIRE(zebra_pos != std::string::npos);
        CHECK(alpha_pos < zebra_pos);

        std::filesystem::remove_all(temp_dir);
    };

} // namespace
