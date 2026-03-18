#include "features/skills/skill-loader.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <cstdlib>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

namespace fs = std::filesystem;

fs::path fixtures_dir() {
    return fs::path(SOURCE_DIR) / "tests/fixtures/skills";
}

fs::path override_dir() {
    return fs::path(SOURCE_DIR) / "tests/fixtures/skills-override";
}

class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, const std::string &value)
    : name_(std::move(name)) {
        if (const char *existing = std::getenv(name_.c_str())) {
            original_value_ = existing;
        }
        if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error("setenv failed");
        }
    }

    ~ScopedEnvVar() {
        if (original_value_.has_value()) {
            EXPECT_EQ(setenv(name_.c_str(), original_value_->c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv(name_.c_str()), 0);
        }
    }

    ScopedEnvVar(const ScopedEnvVar &) = delete;
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;
    ScopedEnvVar(ScopedEnvVar &&) = delete;
    ScopedEnvVar &operator=(ScopedEnvVar &&) = delete;

private:
    std::string name_;
    std::optional<std::string> original_value_;
};

fs::path make_temp_dir(const std::string &name) {
    const auto dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_skill(const fs::path &root, const std::string &dir_name, const std::string &content) {
    const auto skill_dir = root / dir_name;
    fs::create_directories(skill_dir);
    std::ofstream out(skill_dir / "SKILL.md");
    out << content;
}

} // namespace

TEST(SkillLoaderTest, LoadsValidSkill) {
    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    bool found = false;
    for (const auto &skill : loader.active_skills()) {
        if (skill.name == "test-skill") {
            found = true;
            EXPECT_EQ(skill.description, "A test skill for unit testing");
            EXPECT_EQ(skill.tools.size(), 2);
            EXPECT_EQ(skill.tools[0], "read");
            EXPECT_EQ(skill.tools[1], "grep");
            EXPECT_TRUE(skill.body.find("test skill body") != std::string::npos);
        }
    }
    EXPECT_TRUE(found) << "Expected 'test-skill' to be loaded";
}

TEST(SkillLoaderTest, SkipsMissingNameField) {
    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    for (const auto &skill : loader.active_skills()) {
        EXPECT_NE(skill.description, "Missing the name field") << "Skill with missing name should not be loaded";
    }
}

TEST(SkillLoaderTest, SkipsBadToml) {
    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    for (const auto &skill : loader.active_skills()) {
        EXPECT_NE(skill.name, "invalid toml here [[[") << "Skill with bad TOML should not be loaded";
    }
}

TEST(SkillLoaderTest, SkipsUnmetEnvDependency) {
    // Make sure the env var is NOT set
    unsetenv("ORANGUTAN_TEST_ENV_SKILL");

    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    for (const auto &skill : loader.active_skills()) {
        EXPECT_NE(skill.name, "env-skill") << "Skill with unmet env should not be loaded";
    }
}

TEST(SkillLoaderTest, LoadsSkillWithMetEnvDependency) {
    ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "1");

    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    bool found = false;
    for (const auto &skill : loader.active_skills()) {
        if (skill.name == "env-skill") {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Skill with met env should be loaded";
}

TEST(SkillLoaderTest, ScopedEnvVarRestoresPreviousValue) {
    ASSERT_EQ(setenv("ORANGUTAN_TEST_ENV_SKILL", "original", 1), 0);

    {
        ScopedEnvVar env("ORANGUTAN_TEST_ENV_SKILL", "temporary");
        const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
        ASSERT_NE(value, nullptr);
        EXPECT_STREQ(value, "temporary");
    }

    const char *value = std::getenv("ORANGUTAN_TEST_ENV_SKILL");
    ASSERT_NE(value, nullptr);
    EXPECT_STREQ(value, "original");

    ASSERT_EQ(unsetenv("ORANGUTAN_TEST_ENV_SKILL"), 0);
}

TEST(SkillLoaderTest, LaterDirectoryShadowsSameName) {
    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string(), override_dir().string()});

    for (const auto &skill : loader.active_skills()) {
        if (skill.name == "test-skill") {
            EXPECT_EQ(skill.description, "Overridden version of test-skill");
            EXPECT_TRUE(skill.body.find("workspace override") != std::string::npos);
        }
    }
}

TEST(SkillLoaderTest, NonexistentDirectoryIsSkipped) {
    SkillLoader loader;
    loader.load_from_directories({"/nonexistent/path/that/does/not/exist"});
    EXPECT_TRUE(loader.active_skills().empty());
}

TEST(SkillLoaderTest, BuildPromptSectionEmpty) {
    SkillLoader loader;
    loader.load_from_directories({"/nonexistent"});
    EXPECT_TRUE(loader.build_prompt_section().empty());
}

TEST(SkillLoaderTest, BuildPromptSectionContainsSkills) {
    SkillLoader loader;
    loader.load_from_directories({fixtures_dir().string()});

    auto section = loader.build_prompt_section();
    EXPECT_TRUE(section.find("## Active Skills") != std::string::npos);
    EXPECT_TRUE(section.find("### test-skill") != std::string::npos);
}

TEST(SkillLoaderTest, LoadsFrontmatterWithEmbeddedDelimiterInTomlString) {
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

    ASSERT_EQ(loader.active_skills().size(), 1);
    EXPECT_EQ(loader.active_skills()[0].name, "embedded-delimiter");
    EXPECT_TRUE(loader.active_skills()[0].body.find("Body with embedded delimiter") != std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(SkillLoaderTest, LoadsFrontmatterWithDelimiterLineInsideTomlMultilineString) {
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

    ASSERT_EQ(loader.active_skills().size(), 1);
    EXPECT_EQ(loader.active_skills()[0].name, "multiline-delimiter");
    EXPECT_TRUE(loader.active_skills()[0].body.find("Body after multiline string frontmatter.") != std::string::npos);

    fs::remove_all(temp_dir);
}

TEST(SkillLoaderTest, LoadsFrontmatterWithUtf8BomBeforeOpeningDelimiter) {
    const auto temp_dir = make_temp_dir("orangutan_skill_loader_bom");
    write_skill(temp_dir, "bom-skill", std::string("\xEF\xBB\xBF") + R"md(+++
name = "bom-skill"
description = "Frontmatter starts after a UTF-8 BOM"
+++
Body after BOM.
)md");

    SkillLoader loader;
    loader.load_from_directories({temp_dir.string()});

    ASSERT_EQ(loader.active_skills().size(), 1);
    EXPECT_EQ(loader.active_skills()[0].name, "bom-skill");

    fs::remove_all(temp_dir);
}

TEST(SkillLoaderTest, LoadsFrontmatterWithLeadingBlankLineBeforeOpeningDelimiter) {
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

    ASSERT_EQ(loader.active_skills().size(), 1);
    EXPECT_EQ(loader.active_skills()[0].name, "blank-line-skill");

    fs::remove_all(temp_dir);
}

TEST(SkillLoaderTest, ActiveSkillsAreSortedByNameAfterShadowing) {
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

    ASSERT_EQ(loader.active_skills().size(), 2);
    EXPECT_EQ(loader.active_skills()[0].name, "alpha");
    EXPECT_EQ(loader.active_skills()[0].description, "alpha override");
    EXPECT_EQ(loader.active_skills()[1].name, "zebra");

    fs::remove_all(base_dir);
    fs::remove_all(override_root);
}

TEST(SkillLoaderTest, BuildPromptSectionUsesDeterministicSkillOrder) {
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
    ASSERT_NE(alpha_pos, std::string::npos);
    ASSERT_NE(zebra_pos, std::string::npos);
    EXPECT_LT(alpha_pos, zebra_pos);

    fs::remove_all(temp_dir);
}
