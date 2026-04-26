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

    TEST_CASE("save_writes_profiles_models_and_agent_profile_references") {
        orangutan::Config cfg;
        cfg.profiles["gateway-a"] = orangutan::ProfileConfig{
            .base_url = "https://gateway.example.com",
            .api_key = "sk-test",
            .headers = {{"X-App-Id", "orangutan"}},
            .models =
                {
                    {"gpt-4.1",
                     orangutan::ModelConfig{
                         .provider = "openai",
                         .protocol = "responses",
                         .max_tokens = 32000,
                         .context_window = 128000,
                         .thinking = "medium",
                         .cost =
                             orangutan::ModelCostConfig{
                                 .input = 2.0,
                                 .output = 8.0,
                             },
                     }},
                },
        };
        cfg.agents["default"] = orangutan::AgentConfig{
            .profile = "gateway-a",
            .model = "gpt-4.1",
            .fallback_models =
                {
                    "gpt-4.1-mini",
                    orangutan::FallbackModelRef{"gateway-b", "claude-sonnet-4-20250514"},
                },
            .workspace = "~/workspace/default",
        };

        const auto path = orangutan::testing::unique_test_path("config-save", "config.json");
        cfg.save_to(path);

        const auto stored = nlohmann::json::parse(read_file(path));
        CHECK(stored["profiles"]["gateway-a"]["base_url"] == "https://gateway.example.com");
        CHECK(stored["profiles"]["gateway-a"]["api_key"] == "sk-test");
        CHECK(stored["profiles"]["gateway-a"]["models"]["gpt-4.1"]["provider"] == "openai");
        CHECK(stored["profiles"]["gateway-a"]["models"]["gpt-4.1"]["protocol"] == "responses");
        CHECK(stored["profiles"]["gateway-a"]["models"]["gpt-4.1"]["thinking"] == "medium");
        CHECK(stored["agents"]["default"]["profile"] == "gateway-a");
        CHECK(stored["agents"]["default"]["model"] == "gpt-4.1");
        CHECK(stored["agents"]["default"]["fallback_models"][0] == "gpt-4.1-mini");
        CHECK(stored["agents"]["default"]["fallback_models"][1]["profile"] == "gateway-b");
        CHECK(stored["agents"]["default"]["fallback_models"][1]["model"] == "claude-sonnet-4-20250514");

        const auto loaded = orangutan::Config::load_from(path);
        CHECK(loaded.agents.at("default").profile == "gateway-a");
        CHECK(loaded.profiles.at("gateway-a").models.at("gpt-4.1").provider == "openai");
        CHECK(loaded.profiles.at("gateway-a").models.at("gpt-4.1").protocol == "responses");

        std::filesystem::remove_all(path.parent_path());
    };

    TEST_CASE("save_writes_only_new_permissions_format") {
        orangutan::Config cfg;
        cfg.permissions_config = orangutan::PermissionConfig{
            .default_mode = orangutan::permission_mode::accept_edits,
            .allow = {"read"},
            .deny = {"shell(rm:*)"},
            .ask = {"edit"},
        };
        cfg.agents["default"] = orangutan::AgentConfig{
            .permissions_config =
                {
                    .default_mode = orangutan::permission_mode::plan,
                    .allow = {"read"},
                    .ask = {"shell"},
                },
        };

        const auto path = orangutan::testing::unique_test_path("config-save-permissions", "config.json");
        cfg.save_to(path);

        const auto stored = nlohmann::json::parse(read_file(path));
        CHECK(stored["tools"]["edit_mode"] == "hashline");
        CHECK_FALSE(stored["tools"].contains("allowed"));
        CHECK_FALSE(stored["tools"].contains("denied"));
        CHECK(stored["permissions"]["default_mode"] == "accept_edits");
        CHECK(stored["permissions"]["allow"] == nlohmann::json::array({"read"}));
        CHECK(stored["permissions"]["deny"] == nlohmann::json::array({"shell(rm:*)"}));
        CHECK(stored["permissions"]["ask"] == nlohmann::json::array({"edit"}));
        CHECK(stored["agents"]["default"]["permissions"]["default_mode"] == "plan");
        CHECK(stored["agents"]["default"]["permissions"]["allow"] == nlohmann::json::array({"read"}));
        CHECK(stored["agents"]["default"]["permissions"]["ask"] == nlohmann::json::array({"shell"}));

        std::filesystem::remove_all(path.parent_path());
    };

} // namespace
