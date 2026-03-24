#include "infra/config/secret-protection.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include "support/ut.hpp"

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

boost::ut::suite config_secret_suite = [] {
    using namespace boost::ut;

    "protect_and_reveal_round_trip"_test = [] {
        const auto stored = orangutan::protect_config_secret("sk-secret-123", "correct horse battery staple", "agent.api_key");
        expect(orangutan::is_protected_config_secret(stored));

        const auto revealed = orangutan::reveal_config_secret(stored, "correct horse battery staple", "agent.api_key", "agent.api_key");
        expect(revealed == "sk-secret-123");
    };

    "rejects_wrong_password_without_leaking_secret"_test = [] {
        const auto stored = orangutan::protect_config_secret("top-secret-token", "right-password", "agents.api_key");

        try {
            static_cast<void>(orangutan::reveal_config_secret(stored, "wrong-password", "agents.api_key", "agents.coder.api_key"));
            expect(false >> fatal) << "expected protected secret decryption to fail";
        } catch (const orangutan::ConfigSecretProtectionError &e) {
            const std::string message = e.what();
            expect(message.find("agents.coder.api_key") != std::string::npos);
            expect(message.find("top-secret-token") == std::string::npos);
            expect(message.find(stored) == std::string::npos);
        }
    };

    "rejects_missing_password_for_protected_secret"_test = [] {
        const auto stored = orangutan::protect_config_secret("top-secret-token", "right-password", "agents.api_key");
        expect(throws<orangutan::ConfigSecretProtectionError>([&] {
            static_cast<void>(orangutan::reveal_config_secret(stored, "", "agents.api_key", "agents.coder.api_key"));
        }));
    };

    "rejects_malformed_payload_without_echoing_payload"_test = [] {
        try {
            static_cast<void>(orangutan::reveal_config_secret("enc:v1:not-valid-***", "password", "agent.api_key", "agent.api_key"));
            expect(false >> fatal) << "expected malformed payload to fail";
        } catch (const orangutan::ConfigSecretProtectionError &e) {
            const std::string message = e.what();
            expect(message.find("not-valid-***") == std::string::npos);
        }
    };

    "field_binding_prevents_replay_across_secret_kinds"_test = [] {
        const auto stored = orangutan::protect_config_secret("qq-secret", "shared-password", "qq.client_secret");
        expect(throws<orangutan::ConfigSecretProtectionError>([&] {
            static_cast<void>(orangutan::reveal_config_secret(stored, "shared-password", "agents.api_key", "agents.default.api_key"));
        }));
    };

    "protect_flow_encrypts_eligible_secrets_and_skips_environment_references"_test = [] {
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
        expect(result.modified >> fatal) << "expected protect_config_file_secrets to modify the config";
        expect(result.protected_count == 2_ul);
        expect(std::filesystem::exists(result.backup_path));

        const auto updated = ConfigSecretFileHarness::read_config(harness.config_path());
        expect(updated.find("enc:v1:") != std::string::npos);
        expect(updated.find("api_key = \"${CODER_KEY}\"") != std::string::npos);
        expect(updated.find("# keep comment") != std::string::npos);
        expect(updated.find("mirror_file = \"notes/MEMORY.md\"") != std::string::npos);

        const auto backup = ConfigSecretFileHarness::read_config(result.backup_path);
        expect(backup.find("plain-agent-key") != std::string::npos);
        expect(backup.find("plain-qq-secret") != std::string::npos);
    };

    "protect_flow_preserves_config_permissions"_test = [] {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

        std::filesystem::permissions(harness.config_path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
        const auto original_permissions = std::filesystem::status(harness.config_path()).permissions();

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        expect(result.modified >> fatal) << "expected protect_config_file_secrets to modify the config";
        expect(std::filesystem::status(harness.config_path()).permissions() == original_permissions);
    };

    "protect_flow_skips_nested_agent_tables"_test = [] {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agents.coder]
api_key = "plain-agent-key"

[agents.coder.extra]
api_key = "nested-agent-key"
)toml");

        const auto result = orangutan::protect_config_file_secrets(harness.config_path(), "protect-password");
        expect(result.modified >> fatal) << "expected protect_config_file_secrets to modify the config";
        expect(result.protected_count == 1_ul);

        const auto updated = ConfigSecretFileHarness::read_config(harness.config_path());
        expect(updated.find("[agents.coder]\napi_key = \"enc:v1:") != std::string::npos);
        expect(updated.find("[agents.coder.extra]\napi_key = \"nested-agent-key\"") != std::string::npos);
    };

    "protect_flow_leaves_file_untouched_on_failure"_test = [] {
        ConfigSecretFileHarness harness;
        harness.write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

        const auto original = ConfigSecretFileHarness::read_config(harness.config_path());
        expect(throws<orangutan::ConfigSecretProtectionError>([&] {
            static_cast<void>(orangutan::protect_config_file_secrets(harness.config_path(), ""));
        }));
        expect(ConfigSecretFileHarness::read_config(harness.config_path()) == original);
        expect(not std::filesystem::exists(harness.config_path().string() + ".bak"));
    };
};

} // namespace
