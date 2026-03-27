#include "infra/config/secret-protection.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace {

    class ConfigSecretFileHarness {
    public:
        ConfigSecretFileHarness()
        : root_(orangutan::testing::unique_test_root("config-secret")) {}

        ~ConfigSecretFileHarness() {
            std::filesystem::remove_all(root_);
        }

        [[nodiscard]]
        std::filesystem::path config_path() const {
            return root_ / "config.toml";
        }

        void write_config(const std::string &content) const {
            std::ofstream out(config_path());
            out << content;
        }

        [[nodiscard]]
        static std::string read_config(const std::filesystem::path &path) {
            std::ifstream in(path);
            return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        }

    private:
        std::filesystem::path root_;
    };

    TEST_CASE("protect_and_reveal_round_trip") {
        const auto stored = orangutan::protect_config_secret("sk-secret-123", "correct horse battery staple", "agent.api_key");
        CHECK(orangutan::is_protected_config_secret(stored));

        const auto revealed = orangutan::reveal_config_secret(stored, "correct horse battery staple", "agent.api_key", "agent.api_key");
        CHECK(revealed == "sk-secret-123");
    };

    TEST_CASE("rejects_wrong_password_without_leaking_secret") {
        const auto stored = orangutan::protect_config_secret("top-secret-token", "right-password", "agents.api_key");

        try {
            static_cast<void>(orangutan::reveal_config_secret(stored, "wrong-password", "agents.api_key", "agents.coder.api_key"));
            FAIL("expected protected secret decryption to fail");
        } catch (const orangutan::ConfigSecretProtectionError &e) {
            const std::string message = e.what();
            CHECK(message.contains("agents.coder.api_key"));
            CHECK_FALSE(message.contains("top-secret-token"));
            CHECK_FALSE(message.contains(stored));
        }
    };

    TEST_CASE("rejects_missing_password_for_protected_secret") {
        const auto stored = orangutan::protect_config_secret("top-secret-token", "right-password", "agents.api_key");
        REQUIRE_THROWS_AS(orangutan::reveal_config_secret(stored, "", "agents.api_key", "agents.coder.api_key"), orangutan::ConfigSecretProtectionError);
    };

    TEST_CASE("rejects_malformed_payload_without_echoing_payload") {
        try {
            static_cast<void>(orangutan::reveal_config_secret("enc:v1:not-valid-***", "password", "agent.api_key", "agent.api_key"));
            FAIL("expected malformed payload to fail");
        } catch (const orangutan::ConfigSecretProtectionError &e) {
            const std::string message = e.what();
            CHECK_FALSE(message.contains("not-valid-***"));
        }
    };

    TEST_CASE("field_binding_prevents_replay_across_secret_kinds") {
        const auto stored = orangutan::protect_config_secret("qq-secret", "shared-password", "qq.client_secret");
        REQUIRE_THROWS_AS(orangutan::reveal_config_secret(stored, "shared-password", "agents.api_key", "agents.default.api_key"), orangutan::ConfigSecretProtectionError);
    };

    TEST_CASE("protect_flow_encrypts_eligible_secrets_and_skips_environment_references") {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agent]
api_key = "plain-agent-key" # keep comment

[agents.coder]
api_key = "${CODER_KEY}"

[qq]
client_secret = "plain-qq-secret"

[memory]
mirror_file = "notes/MEMORY.md"
)toml");

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        INFO("expected protect_config_file_secrets to modify the config");
        REQUIRE(result.modified);
        CHECK(result.protected_count == 2ul);
        CHECK(std::filesystem::exists(result.backup_path));

        const auto updated = ConfigSecretFileHarness::read_config(harness.config_path());
        CHECK(updated.contains("enc:v1:"));
        CHECK(updated.contains("api_key = \"${CODER_KEY}\""));
        CHECK(updated.contains("# keep comment"));
        CHECK(updated.contains("mirror_file = \"notes/MEMORY.md\""));

        const auto backup = ConfigSecretFileHarness::read_config(result.backup_path);
        CHECK(backup.contains("plain-agent-key"));
        CHECK(backup.contains("plain-qq-secret"));
    };

    TEST_CASE("protect_flow_preserves_config_permissions") {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

        std::filesystem::permissions(harness.config_path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
        const auto original_permissions = std::filesystem::status(harness.config_path()).permissions();

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        INFO("expected protect_config_file_secrets to modify the config");
        REQUIRE(result.modified);
        CHECK(std::filesystem::status(harness.config_path()).permissions() == original_permissions);
    };

    TEST_CASE("protect_flow_skips_nested_agent_tables") {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agents.coder]
api_key = "plain-agent-key"

[agents.coder.extra]
api_key = "nested-agent-key"
)toml");

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        INFO("expected protect_config_file_secrets to modify the config");
        REQUIRE(result.modified);
        CHECK(result.protected_count == 1ul);

        const auto updated = ConfigSecretFileHarness::read_config(harness.config_path());
        CHECK(updated.contains("[agents.coder]\napi_key = \"enc:v1:"));
        CHECK(updated.contains("[agents.coder.extra]\napi_key = \"nested-agent-key\""));
    };

    TEST_CASE("protect_flow_leaves_file_untouched_on_failure") {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

        const auto original = ConfigSecretFileHarness::read_config(harness.config_path());
        REQUIRE_THROWS_AS(orangutan::protect_config_file_secrets(harness.config_path(), ""), orangutan::ConfigSecretProtectionError);
        CHECK(ConfigSecretFileHarness::read_config(harness.config_path()) == original);
        CHECK_FALSE(std::filesystem::exists(harness.config_path().string() + ".bak"));
    };

} // namespace
