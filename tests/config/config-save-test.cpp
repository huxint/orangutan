#include "config/config.hpp"
#include "test-helpers.hpp"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

    std::string read_file(const std::filesystem::path &path) {
        std::ifstream in(path);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    TEST_CASE("round_trips_save_and_load") {
        orangutan::Config cfg;
        cfg.model = "test-model-42";
        cfg.temperature = 0.7;
        cfg.max_tokens = 2048;
        cfg.provider = "openai";
        cfg.base_url = "http://localhost:8080";
        cfg.edit_mode = "search_replace";
        cfg.auto_save = false;

        const auto path = orangutan::testing::unique_test_path("config-save", "config.json");
        cfg.save_to(path);

        const auto loaded = orangutan::Config::load_from(path);
        CHECK(loaded.model == "test-model-42");
        CHECK(loaded.temperature == 0.7);
        CHECK(loaded.max_tokens == 2048);
        CHECK(loaded.provider == "openai");
        CHECK(loaded.base_url == "http://localhost:8080");
        CHECK(loaded.edit_mode == "search_replace");
        CHECK_FALSE(loaded.auto_save);
        CHECK(nlohmann::json::parse(read_file(path))["agent"]["model"] == "test-model-42");

        std::filesystem::remove_all(path.parent_path());
    };

    TEST_CASE("preserves_memory_config") {
        orangutan::Config cfg;
        cfg.memory.mirror_enabled = true;
        cfg.memory.mirror_file = "custom.md";
        cfg.memory.journal_dir = "custom-journal";

        const auto path = orangutan::testing::unique_test_path("config-save-memory", "config.json");
        cfg.save_to(path);

        const auto loaded = orangutan::Config::load_from(path);
        CHECK(loaded.memory.mirror_enabled);
        CHECK(loaded.memory.mirror_file == "custom.md");
        CHECK(loaded.memory.journal_dir == "custom-journal");

        std::filesystem::remove_all(path.parent_path());
    };

} // namespace
