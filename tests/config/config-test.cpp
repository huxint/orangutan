#include "config/config.hpp"
#include "config/secret-protection.hpp"
#include "test-helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

namespace {

    class ConfigFileHarness {
    public:
        ConfigFileHarness()
        : tmp_dir_(orangutan::testing::unique_test_root("config-test")) {}

        ~ConfigFileHarness() {
            std::filesystem::remove_all(tmp_dir_);
        }

        [[nodiscard]]
        std::string write_config(const nlohmann::json &content) const {
            const auto path = (tmp_dir_ / "config.json").string();
            std::ofstream ofs(path);
            ofs << content.dump(2);
            return path;
        }

    private:
        std::filesystem::path tmp_dir_;
    };

    TEST_CASE("default_values_use_new_profile_schema") {
        const Config cfg;
        CHECK(cfg.profile.empty());
        CHECK(cfg.model == "claude-sonnet-4-20250514");
        CHECK(cfg.profiles.empty());
        CHECK(cfg.agents.empty());
        CHECK(cfg.temperature == 1.0);
        CHECK(cfg.max_iterations == 20);
        CHECK(cfg.max_tokens == 4096);
        CHECK(cfg.edit_mode == "hashline");
        CHECK(cfg.auto_save);
        CHECK_FALSE(cfg.memory.mirror_enabled);
        CHECK(cfg.memory.mirror_file == "MEMORY.md");
        CHECK(cfg.memory.journal_dir == "memory");
    };

    TEST_CASE("parses_profiles_and_agent_references") {
        ConfigFileHarness harness;
        ScopedEnvVar api_env("ORANGUTAN_PROFILE_KEY", "profile-secret");
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "api_key": "${ORANGUTAN_PROFILE_KEY}",
              "headers": {
                "X-App-Id": "orangutan"
              },
              "models": {
                "gpt-4.1": {
                  "endpoint_style": "openai-responses",
                  "max_tokens": 32000,
                  "context_window": 128000,
                  "thinking": "medium",
                  "cost": {
                    "input": 2.0,
                    "output": 8.0
                  }
                }
              }
            }
          },
          "agents": {
            "default": {
              "profile": "gateway-a",
              "model": "gpt-4.1",
              "fallback_models": ["gpt-4.1-mini"],
              "workspace": "~/workspace/default",
              "system_prompt": "You are the default agent.",
              "subagents": ["coder"]
            }
          }
        })json"));

        const auto cfg = Config::load_from(path);
        REQUIRE(cfg.profiles.contains("gateway-a"));
        const auto &profile = cfg.profiles.at("gateway-a");
        CHECK(profile.base_url == "https://gateway.example.com");
        CHECK(profile.api_key == "profile-secret");
        CHECK(profile.headers.at("X-App-Id") == "orangutan");
        REQUIRE(profile.models.contains("gpt-4.1"));
        const auto &model = profile.models.at("gpt-4.1");
        CHECK(model.endpoint_style == "openai-responses");
        REQUIRE(model.max_tokens.has_value());
        CHECK(*model.max_tokens == 32000);
        REQUIRE(model.context_window.has_value());
        CHECK(*model.context_window == 128000);
        CHECK(model.thinking == "medium");
        REQUIRE(model.cost.has_value());
        CHECK(model.cost->input == 2.0);
        CHECK(model.cost->output == 8.0);

        REQUIRE(cfg.agents.contains("default"));
        const auto &agent = cfg.agents.at("default");
        CHECK(agent.profile == "gateway-a");
        CHECK(agent.model == "gpt-4.1");
        REQUIRE(agent.fallback_models.size() == 1UL);
        CHECK(agent.fallback_models.front() == "gpt-4.1-mini");
        CHECK(agent.system_prompt == "You are the default agent.");
        CHECK(agent.subagents == std::vector<std::string>{"coder"});
        CHECK(agent.workspace.contains("/workspace/default"));
    };

    TEST_CASE("named_agents_inherit_root_agent_defaults_without_synthesizing_default_agent") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "agent": {
            "profile": "shared-profile",
            "model": "shared-model",
            "fallback_models": ["global-fallback"],
            "workspace": "~/workspace/shared",
            "system_prompt": "Shared prompt"
          },
          "agents": {
            "coder": {
              "model": "coder-model"
            }
          }
        })json"));

        const auto cfg = Config::load_from(path);
        CHECK_FALSE(cfg.agents.contains("default"));
        REQUIRE(cfg.agents.contains("coder"));
        const auto &coder = cfg.agents.at("coder");
        CHECK(coder.profile == "shared-profile");
        CHECK(coder.model == "coder-model");
        CHECK(coder.fallback_models == std::vector<std::string>{"global-fallback"});
        CHECK(coder.system_prompt == "Shared prompt");
        CHECK(coder.workspace.contains("/workspace/shared"));
    };

    TEST_CASE("loads_protected_profile_api_key") {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-protected-profile", "profile-password", "profiles.api_key");
        const auto path = harness.write_config(nlohmann::json{
            {"profiles",
             {
                 {"gateway-a",
                  {
                      {"base_url", "https://gateway.example.com"},
                      {"api_key", protected_key},
                      {"models", {{"gpt-4.1", {{"endpoint_style", "openai-responses"}}}}},
                  }},
             }},
        });

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "profile-password",
                                                 });
        REQUIRE(cfg.profiles.contains("gateway-a"));
        CHECK(cfg.profiles.at("gateway-a").api_key == "sk-protected-profile");
    };

    TEST_CASE("invalid_thinking_value_rejects_config") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "endpoint_style": "openai-responses",
                  "thinking": "turbo"
                }
              }
            }
          }
        })json"));

        const auto cfg = Config::load_from(path);
        CHECK(cfg.profiles.empty());
    };

    TEST_CASE("save_round_trips_profiles_and_agents") {
        Config cfg;
        cfg.profile = "shared-profile";
        cfg.model = "shared-model";
        cfg.fallback_models = {"global-fallback"};
        cfg.system_prompt = "Shared prompt";
        cfg.workspace = "~/workspace/shared";
        cfg.profiles["gateway-a"] = ProfileConfig{
            .base_url = "https://gateway.example.com",
            .api_key = "${PROFILE_KEY}",
            .headers = {{"X-App-Id", "orangutan"}},
            .models =
                {
                    {"gpt-4.1",
                     ModelConfig{
                         .endpoint_style = "openai-responses",
                         .max_tokens = 32000,
                         .context_window = 128000,
                         .thinking = "high",
                         .cost =
                             ModelCostConfig{
                                 .input = 2.0,
                                 .output = 8.0,
                             },
                     }},
                },
        };
        cfg.agents["default"] = AgentConfig{
            .profile = "gateway-a",
            .model = "gpt-4.1",
            .fallback_models = {"gpt-4.1-mini"},
            .system_prompt = "Default prompt",
            .workspace = "~/workspace/default",
            .subagents = {"coder"},
        };

        const auto path = orangutan::testing::unique_test_path("config-roundtrip", "config.json");
        cfg.save_to(path);
        const auto loaded = Config::load_from(path);

        REQUIRE(loaded.profiles.contains("gateway-a"));
        REQUIRE(loaded.profiles.at("gateway-a").models.contains("gpt-4.1"));
        CHECK(loaded.agents.at("default").profile == "gateway-a");
        CHECK(loaded.agents.at("default").model == "gpt-4.1");
        REQUIRE(loaded.profiles.at("gateway-a").models.at("gpt-4.1").context_window.has_value());
        CHECK(*loaded.profiles.at("gateway-a").models.at("gpt-4.1").context_window == 128000);
        REQUIRE(loaded.profiles.at("gateway-a").models.at("gpt-4.1").cost.has_value());
        CHECK(loaded.profiles.at("gateway-a").models.at("gpt-4.1").cost->output == 8.0);

        std::filesystem::remove_all(path.parent_path());
    };

} // namespace
