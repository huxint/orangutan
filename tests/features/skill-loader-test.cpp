#include "features/skills/skill-loader.hpp"
#include "test-helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include "support/ut.hpp"

using namespace orangutan;

namespace {

std::filesystem::path fixtures_dir() {
    return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/skills";
}

std::filesystem::path override_dir() {
    return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/skills-override";
}

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

std::filesystem::path make_temp_dir(const std::string &name) {
    return orangutan::testing::unique_test_root(name);
}

void write_skill(const std::filesystem::path &root, const std::string &dir_name, const std::string &content) {
    const auto skill_dir = root / dir_name;
    std::filesystem::create_directories(skill_dir);
    std::ofstream out(skill_dir / "SKILL.md");
    out << content;
}

const SkillDef *find_skill(const SkillLoader &loader, std::string_view name) {
    for (const auto &skill : loader.active_skills()) {
        if (skill.name == name) {
            return &skill;
        }
    }
    return nullptr;
}

boost::ut::suite skill_loader_suite = [] {
    using namespace boost::ut;

    "loads_valid_skill"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto *skill = find_skill(loader, "test-skill");
        expect((skill != nullptr) >> fatal) << "expected 'test-skill' to be loaded";
        expect(skill->description == "A test skill for unit testing");
        expect(skill->tools.size() == 2_ul);
        expect(skill->tools[0] == "read");
        expect(skill->tools[1] == "grep");
        expect(skill->body.contains("test skill body"));
    };

    "skips_missing_name_field"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        for (const auto &skill : loader.active_skills()) {
            expect(skill.description != "Missing the name field") << "skill with missing name should not be loaded";
        }
    };

    "skips_bad_toml"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        for (const auto &skill : loader.active_skills()) {
            expect(skill.name != "invalid toml here [[[") << "skill with bad TOML should not be loaded";
        }
    };

    "skips_unmet_env_dependency"_test = [] {
        unsetenv("ORANGUTAN_TEST_ENV_SKILL");

        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        for (const auto &skill : loader.active_skills()) {
            expect(skill.name != "env-skill") << "skill with unmet env should not be loaded";
        }
    };

    "loads_skill_with_met_env_dependency"_test = [] {
        ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "1");

        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        expect((find_skill(loader, "env-skill") != nullptr) >> fatal) << "skill with met env should be loaded";
    };

    "scoped_env_var_restores_previous_value"_test = [] {
        expect(setenv("ORANGUTAN_TEST_ENV_SKILL", "original", 1) == 0_i);

        {
            ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "temporary");
            const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
            expect((value != nullptr) >> fatal) << "expected env var during scope";
            expect(std::string_view{value} == "temporary");
        }

        const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
        expect((value != nullptr) >> fatal) << "expected env var restored after scope";
        expect(std::string_view{value} == "original");
        expect(unsetenv("ORANGUTAN_TEST_ENV_SKILL") == 0_i);
    };

    "later_directory_shadows_same_name"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string(), override_dir().string()});

        const auto *skill = find_skill(loader, "test-skill");
        expect((skill != nullptr) >> fatal) << "expected overridden skill to exist";
        expect(skill->description == "Overridden version of test-skill");
        expect(skill->body.contains("workspace override"));
    };

    "nonexistent_directory_is_skipped"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({"/nonexistent/path/that/does/not/exist"});
        expect(loader.active_skills().empty());
    };

    "build_prompt_section_empty"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({"/nonexistent"});
        expect(loader.build_prompt_section().empty());
    };

    "build_prompt_section_contains_skills"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto section = loader.build_prompt_section();
        expect(section.contains("## Active Skills"));
        expect(section.contains("### test-skill"));
    };

    "loads_frontmatter_with_embedded_delimiter_in_toml_string"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_embedded_delimiter");
        write_skill(temp_dir, "embedded-delimiter", R"md(+++
name = "embedded-delimiter"
description = "Contains +++ in a TOML string"
tools = ["read"]
example = "before +++ after"
+++
Body with embedded delimiter in frontmatter string.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "embedded-delimiter");
        expect(loader.active_skills()[0].body.contains("Body with embedded delimiter"));

        std::filesystem::remove_all(temp_dir);
    };

    "loads_frontmatter_with_delimiter_line_inside_toml_multiline_string"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_multiline_delimiter");
        write_skill(temp_dir, "multiline-delimiter", R"md(+++
name = "multiline-delimiter"
description = "Contains a delimiter line inside a multiline string"
example = """
before
+++
after
"""
+++
Body after multiline string frontmatter.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "multiline-delimiter");
        expect(loader.active_skills()[0].body.contains("Body after multiline string frontmatter."));

        std::filesystem::remove_all(temp_dir);
    };

    "loads_frontmatter_with_utf8_bom_before_opening_delimiter"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_bom");
        write_skill(temp_dir, "bom-skill", std::string("\xEF\xBB\xBF") + R"md(+++
name = "bom-skill"
description = "Frontmatter starts after a UTF-8 BOM"
+++
Body after BOM.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "bom-skill");

        std::filesystem::remove_all(temp_dir);
    };

    "loads_frontmatter_with_leading_blank_line_before_opening_delimiter"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_leading_blank_line");
        write_skill(temp_dir, "blank-line-skill", R"md(
+++
name = "blank-line-skill"
description = "Frontmatter starts after a leading blank line"
+++
Body after leading blank line.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "blank-line-skill");

        std::filesystem::remove_all(temp_dir);
    };

    "active_skills_are_sorted_by_name_after_shadowing"_test = [] {
        const auto base_dir = make_temp_dir("orangutan_skill_loader_order_base");
        const auto override_root = make_temp_dir("orangutan_skill_loader_order_override");

        write_skill(base_dir, "zebra-dir", R"md(+++
name = "zebra"
description = "zebra base"
+++
zebra body
)md");
        write_skill(base_dir, "alpha-dir", R"md(+++
name = "alpha"
description = "alpha base"
+++
alpha body
)md");
        write_skill(override_root, "alpha-override", R"md(+++
name = "alpha"
description = "alpha override"
+++
alpha override body
)md");

        SkillLoader loader;
        loader.load_from_directories({base_dir.string(), override_root.string()});

        expect(loader.active_skills().size() == 2_ul);
        expect(loader.active_skills()[0].name == "alpha");
        expect(loader.active_skills()[0].description == "alpha override");
        expect(loader.active_skills()[1].name == "zebra");

        std::filesystem::remove_all(base_dir);
        std::filesystem::remove_all(override_root);
    };

    "loads_yaml_frontmatter"_test = [] {
        SkillLoader loader;
        loader.load_from_directories({fixtures_dir().string()});

        const auto *skill = find_skill(loader, "yaml-skill");
        expect((skill != nullptr) >> fatal) << "expected 'yaml-skill' to be loaded";
        expect(skill->description == "A skill using YAML frontmatter");
        expect(skill->tools.size() == 2_ul);
        expect(skill->tools[0] == "read");
        expect(skill->tools[1] == "write");
        expect(skill->body.contains("YAML skill body"));
    };

    "loads_yaml_frontmatter_from_temp_dir"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_yaml");
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

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "my-yaml-skill");
        expect(loader.active_skills()[0].description == "Skill with YAML frontmatter");
        expect(loader.active_skills()[0].tools.size() == 2_ul);
        expect(loader.active_skills()[0].tools[0] == "shell");
        expect(loader.active_skills()[0].tools[1] == "grep");
        expect(loader.active_skills()[0].body.contains("YAML body content"));

        std::filesystem::remove_all(temp_dir);
    };

    "loads_yaml_with_quoted_values"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_yaml_quoted");
        write_skill(temp_dir, "quoted-yaml", R"md(---
name: "quoted-skill"
description: 'Single-quoted description'
tools: ["read", 'write']
---
Quoted values body.
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        expect(loader.active_skills().size() == 1_ul);
        expect(loader.active_skills()[0].name == "quoted-skill");
        expect(loader.active_skills()[0].description == "Single-quoted description");
        expect(loader.active_skills()[0].tools.size() == 2_ul);

        std::filesystem::remove_all(temp_dir);
    };

    "build_prompt_section_uses_deterministic_skill_order"_test = [] {
        const auto temp_dir = make_temp_dir("orangutan_skill_loader_prompt_order");

        write_skill(temp_dir, "zebra-dir", R"md(+++
name = "zebra"
description = "zebra"
+++
zebra prompt body
)md");
        write_skill(temp_dir, "alpha-dir", R"md(+++
name = "alpha"
description = "alpha"
+++
alpha prompt body
)md");

        SkillLoader loader;
        loader.load_from_directories({temp_dir.string()});

        const auto section = loader.build_prompt_section();
        const auto alpha_pos = section.find("### alpha");
        const auto zebra_pos = section.find("### zebra");
        expect((alpha_pos != std::string::npos) >> fatal);
        expect((zebra_pos != std::string::npos) >> fatal);
        expect(alpha_pos < zebra_pos);

        std::filesystem::remove_all(temp_dir);
    };
};

} // namespace
