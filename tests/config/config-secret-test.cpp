#include "config/secret-protection.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

    class ConfigSecretFileHarness {
    public:
        ConfigSecretFileHarness()
        : root_(orangutan::testing::unique_test_root("config-secret")) {}

        ~ConfigSecretFileHarness() {
            std::filesystem::remove_all(root_);
        }

        ConfigSecretFileHarness(const ConfigSecretFileHarness &) = delete;
        ConfigSecretFileHarness &operator=(const ConfigSecretFileHarness &) = delete;
        ConfigSecretFileHarness(ConfigSecretFileHarness &&) = delete;
        ConfigSecretFileHarness &operator=(ConfigSecretFileHarness &&) = delete;

        [[nodiscard]]
        std::filesystem::path config_path() const {
            return root_ / "config.json";
        }

        void write_config(const nlohmann::json &content) const {
            std::ofstream out(config_path());
            out << content.dump(2) << '\n';
        }

        [[nodiscard]]
        static nlohmann::json read_config_json(const std::filesystem::path &path) {
            std::ifstream in(path);
            return nlohmann::json::parse(std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
        }

    private:
        std::filesystem::path root_;
    };

    TEST_CASE("protect_and_reveal_round_trip_for_profile_api_key") {
        const auto stored = orangutan::protect_config_secret("sk-secret-123", "correct horse battery staple", "profiles.api_key");
        CHECK(orangutan::is_protected_config_secret(stored));

        const auto revealed = orangutan::reveal_config_secret(stored, "correct horse battery staple", "profiles.api_key", "profiles.gateway-a.api_key");
        CHECK(revealed == "sk-secret-123");
    };

    TEST_CASE("field_binding_prevents_replay_across_secret_kinds") {
        const auto stored = orangutan::protect_config_secret("qq-secret", "shared-password", "qq.client_secret");
        REQUIRE_THROWS_AS(orangutan::reveal_config_secret(stored, "shared-password", "profiles.api_key", "profiles.gateway-a.api_key"), orangutan::ConfigSecretProtectionError);
    };

    TEST_CASE("protect_flow_encrypts_declared_secret_fields_and_skips_non_secret_values") {
        ConfigSecretFileHarness harness;
        harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "api_key": "plain-profile-key",
              "models": {
                "gpt-4.1": {
                  "endpoint_style": "openai-responses",
                  "max_tokens": 32000
                }
              }
            },
            "gateway-b": {
              "api_key": "${PROFILE_B_KEY}"
            }
          },
          "qq": {
            "client_secret": "plain-qq-secret"
          },
          "qq_bots": [
            {
              "name": "bot-a",
              "client_secret": "plain-bot-secret",
              "agent": "default"
            },
            {
              "name": "bot-b",
              "client_secret": "${BOT_SECRET}",
              "agent": "default"
            }
          ]
        })json"));

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        REQUIRE(result.modified);
        CHECK(result.protected_count == 3UL);

        const auto updated = ConfigSecretFileHarness::read_config_json(harness.config_path());
        CHECK(updated["profiles"]["gateway-a"]["api_key"].get<std::string>().starts_with("enc:v1:"));
        CHECK(updated["profiles"]["gateway-b"]["api_key"] == "${PROFILE_B_KEY}");
        CHECK(updated["profiles"]["gateway-a"]["models"]["gpt-4.1"]["max_tokens"] == 32000);
        CHECK(updated["qq"]["client_secret"].get<std::string>().starts_with("enc:v1:"));
        CHECK(updated["qq_bots"][0]["client_secret"].get<std::string>().starts_with("enc:v1:"));
        CHECK(updated["qq_bots"][1]["client_secret"] == "${BOT_SECRET}");
    };

    TEST_CASE("protect_flow_preserves_config_permissions") {
        ConfigSecretFileHarness harness;
        harness.write_config(nlohmann::json::parse(R"json({
          "profiles": {
            "gateway-a": {
              "api_key": "plain-profile-key"
            }
          }
        })json"));

        std::filesystem::permissions(harness.config_path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
        const auto original_permissions = std::filesystem::status(harness.config_path()).permissions();

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        REQUIRE(result.modified);
        CHECK(std::filesystem::status(harness.config_path()).permissions() == original_permissions);
    };

} // namespace
