#include "config/config.hpp"
#include "config/secret-protection.hpp"
#include "test-helpers.hpp"

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

        ConfigFileHarness(const ConfigFileHarness &) = delete;
        ConfigFileHarness &operator=(const ConfigFileHarness &) = delete;
        ConfigFileHarness(ConfigFileHarness &&) = delete;
        ConfigFileHarness &operator=(ConfigFileHarness &&) = delete;

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
        CHECK(cfg.memory.mirror_file == ".orangutan/memory/MEMORY.md");
        CHECK(cfg.memory.journal_dir == ".orangutan/memory/journal");
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
                  "provider": "openai",
                  "protocol": "responses",
                  "max_tokens": 32000,
                  "context_window": 128000,
                  "thinking": "xhigh",
                  "cost": {
                    "input": 2.0,
                    "output": 8.0
                  }
                }
              }
            },
            "gateway-b": {
              "base_url": "https://anthropic.example.com",
              "models": {
                "claude-sonnet-4-20250514": {
                  "provider": "anthropic",
                  "protocol": "messages"
                }
              }
            }
          },
          "agents": {
            "default": {
              "profile": "gateway-a",
              "model": "gpt-4.1",
              "fallback_models": [
                "gpt-4.1-mini",
                {"profile": "gateway-b", "model": "claude-sonnet-4-20250514"}
              ],
              "workspace": "~/workspace/default",
              "team_agents": ["coder"]
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
        CHECK(model.provider == "openai");
        CHECK(model.protocol == "responses");
        REQUIRE(model.max_tokens.has_value());
        CHECK(*model.max_tokens == 32000);
        REQUIRE(model.context_window.has_value());
        CHECK(*model.context_window == 128000);
        CHECK(model.thinking == "xhigh");
        REQUIRE(model.cost.has_value());
        CHECK(model.cost->input == 2.0);
        CHECK(model.cost->output == 8.0);

        REQUIRE(cfg.agents.contains("default"));
        const auto &agent = cfg.agents.at("default");
        CHECK(agent.profile == "gateway-a");
        CHECK(agent.model == "gpt-4.1");
        REQUIRE(agent.fallback_models.size() == 2UL);
        CHECK(agent.fallback_models[0].profile.empty());
        CHECK(agent.fallback_models[0].model == "gpt-4.1-mini");
        CHECK(agent.fallback_models[1].profile == "gateway-b");
        CHECK(agent.fallback_models[1].model == "claude-sonnet-4-20250514");
        CHECK(agent.team_agents == std::vector<std::string>{"coder"});
        CHECK(agent.workspace.contains("/workspace/default"));
    };

    TEST_CASE("named_agents_inherit_root_agent_defaults_without_synthesizing_default_agent") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "agent": {
            "profile": "shared-profile",
            "model": "shared-model",
            "fallback_models": ["global-fallback"],
            "workspace": "~/workspace/shared"
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
        REQUIRE(coder.fallback_models.size() == 1UL);
        CHECK(coder.fallback_models[0].profile.empty());
        CHECK(coder.fallback_models[0].model == "global-fallback");
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
                      {"models", {{"gpt-4.1", {{"provider", "openai"}, {"protocol", "responses"}}}}},
                  }},
             }},
        });

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "profile-password",
                                                 });
        REQUIRE(cfg.profiles.contains("gateway-a"));
        CHECK(cfg.profiles.at("gateway-a").api_key == "sk-protected-profile");
    };

    TEST_CASE("loads_protected_qq_and_qq_bot_client_secrets") {
        ConfigFileHarness harness;
        const auto protected_qq_secret = protect_config_secret("qq-protected-secret", "qq-password", "qq.client_secret");
        const auto protected_bot_secret = protect_config_secret("qq-bot-protected-secret", "qq-password", "qq_bots.client_secret");
        const auto path = harness.write_config(nlohmann::json{
            {"agent", {{"model", "shared-model"}}},
            {"qq",
             {
                 {"app_id", "qq-app-id"},
                 {"client_secret", protected_qq_secret},
             }},
            {"qq_bots",
             {
                 {
                     {"name", "bot-a"},
                     {"app_id", "bot-app-id"},
                     {"client_secret", protected_bot_secret},
                     {"agent", "default"},
                 },
             }},
            {"agents",
             {
                 {"default", {{"model", "shared-model"}}},
             }},
        });

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "qq-password",
                                                 });
        CHECK(cfg.qq_client_secret == "qq-protected-secret");
        REQUIRE(cfg.qq_bots.size() == 1UL);
        CHECK(cfg.qq_bots[0].client_secret == "qq-bot-protected-secret");
    };

    TEST_CASE("invalid_thinking_value_rejects_config") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "provider": "openai",
                  "protocol": "responses",
                  "thinking": "turbo"
                }
              }
            }
          }
        })json"));

        CHECK_THROWS_AS(Config::load_from(path), std::runtime_error);
    };

    TEST_CASE("missing_provider_rejects_config") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "protocol": "responses"
                }
              }
            }
          }
        })json"));

        CHECK_THROWS_AS(Config::load_from(path), std::runtime_error);
    };

    TEST_CASE("missing_protocol_rejects_config") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "provider": "openai"
                }
              }
            }
          }
        })json"));

        CHECK_THROWS_AS(Config::load_from(path), std::runtime_error);
    };

    TEST_CASE("invalid_provider_or_protocol_rejects_config") {
        ConfigFileHarness harness;
        const auto invalid_provider_path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "provider": "made-up",
                  "protocol": "responses"
                }
              }
            }
          }
        })json"));

        CHECK_THROWS_AS(Config::load_from(invalid_provider_path), std::runtime_error);

        const auto invalid_protocol_path = harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "base_url": "https://gateway.example.com",
              "models": {
                "gpt-4.1": {
                  "provider": "openai",
                  "protocol": "made-up"
                }
              }
            }
          }
        })json"));

        CHECK_THROWS_AS(Config::load_from(invalid_protocol_path), std::runtime_error);
    };

    TEST_CASE("save_round_trips_profiles_and_agents") {
        Config cfg;
        cfg.profile = "shared-profile";
        cfg.model = "shared-model";
        cfg.fallback_models = {"global-fallback"};
        cfg.workspace = "~/workspace/shared";
        cfg.profiles["gateway-a"] = ProfileConfig{
            .base_url = "https://gateway.example.com",
            .api_key = "${PROFILE_KEY}",
            .headers = {{"X-App-Id", "orangutan"}},
            .models =
                {
                    {"gpt-4.1",
                     ModelConfig{
                         .provider = "openai",
                         .protocol = "responses",
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
            .workspace = "~/workspace/default",
            .team_agents = {"coder"},
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

    TEST_CASE("parses_new_permissions_format_for_root_and_agents") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "permissions": {
            "default_mode": "accept-edits",
            "allow": ["read", "task(list)"],
            "deny": ["shell(rm:*)"],
            "ask": ["edit", "write"]
          },
          "agents": {
            "default": {
              "permissions": {
                "default_mode": "plan",
                "allow": ["read"],
                "ask": ["shell"]
              }
            }
          }
        })json"));

        const auto cfg = Config::load_from(path);
        CHECK(cfg.permissions_config.default_mode == permission_mode::accept_edits);
        CHECK(cfg.permissions_config.allow == std::vector<std::string>{"read", "task(list)"});
        CHECK(cfg.permissions_config.deny == std::vector<std::string>{"shell(rm:*)"});
        CHECK(cfg.permissions_config.ask == std::vector<std::string>{"edit", "write"});
        REQUIRE(cfg.agents.contains("default"));
        CHECK(cfg.agents.at("default").permissions_config.default_mode == permission_mode::plan);
        CHECK(cfg.agents.at("default").permissions_config.allow == std::vector<std::string>{"read"});
        CHECK(cfg.agents.at("default").permissions_config.ask == std::vector<std::string>{"shell"});
    };

    TEST_CASE("deprecated_permission_keys_still_map_into_new_permission_config") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(nlohmann::json::parse(R"json({
          "tools": {
            "allowed": ["read", "task(list)"],
            "denied": ["write"]
          },
          "permissions": {
            "allowed_tools": ["shell(git:*)"],
            "denied_tools": ["shell(rm:*)"]
          }
        })json"));

        const auto cfg = Config::load_from(path);
        CHECK(cfg.permissions_config.allow == std::vector<std::string>{"shell(git:*)"});
        CHECK(cfg.permissions_config.deny == std::vector<std::string>{"shell(rm:*)"});
    };

} // namespace
