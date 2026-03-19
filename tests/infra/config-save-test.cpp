#include "infra/config/config.hpp"
#include <gtest/gtest.h>
#include <filesystem>

TEST(ConfigSaveTest, RoundTripsSaveAndLoad) {
    orangutan::Config cfg;
    cfg.model = "test-model-42";
    cfg.temperature = 0.7;
    cfg.max_tokens = 2048;
    cfg.provider = "openai";
    cfg.base_url = "http://localhost:8080";
    cfg.edit_mode = "search_replace";
    cfg.auto_save = false;

    auto path = "/tmp/orangutan-config-save-test.toml";
    cfg.save_to(path);

    auto loaded = orangutan::Config::load_from(path);
    EXPECT_EQ(loaded.model, "test-model-42");
    EXPECT_DOUBLE_EQ(loaded.temperature, 0.7);
    EXPECT_EQ(loaded.max_tokens, 2048);
    EXPECT_EQ(loaded.provider, "openai");
    EXPECT_EQ(loaded.base_url, "http://localhost:8080");
    EXPECT_EQ(loaded.edit_mode, "search_replace");
    EXPECT_FALSE(loaded.auto_save);

    std::filesystem::remove(path);
}

TEST(ConfigSaveTest, PreservesMemoryConfig) {
    orangutan::Config cfg;
    cfg.memory.mirror_enabled = true;
    cfg.memory.mirror_file = "custom.md";
    cfg.memory.journal_dir = "custom-journal";

    auto path = "/tmp/orangutan-config-save-memory-test.toml";
    cfg.save_to(path);

    auto loaded = orangutan::Config::load_from(path);
    EXPECT_TRUE(loaded.memory.mirror_enabled);
    EXPECT_EQ(loaded.memory.mirror_file, "custom.md");
    EXPECT_EQ(loaded.memory.journal_dir, "custom-journal");

    std::filesystem::remove(path);
}
