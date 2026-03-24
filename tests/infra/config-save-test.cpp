#include "infra/config/config.hpp"
#include "test-helpers.hpp"
#include "support/ut.hpp"

#include <filesystem>

namespace {

boost::ut::suite config_save_suite = [] {
    using namespace boost::ut;

    "round_trips_save_and_load"_test = [] {
        orangutan::Config cfg;
        cfg.model = "test-model-42";
        cfg.temperature = 0.7;
        cfg.max_tokens = 2048;
        cfg.provider = "openai";
        cfg.base_url = "http://localhost:8080";
        cfg.edit_mode = "search_replace";
        cfg.auto_save = false;

        const auto path = orangutan::testing::unique_test_path("config-save", "config.toml");
        cfg.save_to(path);

        const auto loaded = orangutan::Config::load_from(path);
        expect(loaded.model == "test-model-42");
        expect(loaded.temperature == 0.7_d);
        expect(loaded.max_tokens == 2048_i);
        expect(loaded.provider == "openai");
        expect(loaded.base_url == "http://localhost:8080");
        expect(loaded.edit_mode == "search_replace");
        expect(not loaded.auto_save);

        std::filesystem::remove_all(path.parent_path());
    };

    "preserves_memory_config"_test = [] {
        orangutan::Config cfg;
        cfg.memory.mirror_enabled = true;
        cfg.memory.mirror_file = "custom.md";
        cfg.memory.journal_dir = "custom-journal";

        const auto path = orangutan::testing::unique_test_path("config-save-memory", "config.toml");
        cfg.save_to(path);

        const auto loaded = orangutan::Config::load_from(path);
        expect(loaded.memory.mirror_enabled);
        expect(loaded.memory.mirror_file == "custom.md");
        expect(loaded.memory.journal_dir == "custom-journal");

        std::filesystem::remove_all(path.parent_path());
    };
};

} // namespace
