#include "infra/config/secret-protection.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace orangutan;

namespace {

class ConfigSecretFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "orangutan_config_secret_test";
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }

    void TearDown() override {
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

TEST(ConfigSecretProtectionTest, ProtectAndRevealRoundTrip) {
    const auto stored = protect_config_secret("sk-secret-123", "correct horse battery staple", "agent.api_key");
    EXPECT_TRUE(is_protected_config_secret(stored));

    const auto revealed = reveal_config_secret(stored, "correct horse battery staple", "agent.api_key", "agent.api_key");
    EXPECT_EQ(revealed, "sk-secret-123");
}

TEST(ConfigSecretProtectionTest, RejectsWrongPasswordWithoutLeakingSecret) {
    const auto stored = protect_config_secret("top-secret-token", "right-password", "agents.api_key");

    try {
        (void)reveal_config_secret(stored, "wrong-password", "agents.api_key", "agents.coder.api_key");
        FAIL() << "expected protected secret decryption to fail";
    } catch (const ConfigSecretProtectionError &e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("agents.coder.api_key"), std::string::npos);
        EXPECT_EQ(message.find("top-secret-token"), std::string::npos);
        EXPECT_EQ(message.find(stored), std::string::npos);
    }
}

TEST(ConfigSecretProtectionTest, RejectsMissingPasswordForProtectedSecret) {
    const auto stored = protect_config_secret("top-secret-token", "right-password", "agents.api_key");
    EXPECT_THROW((void)reveal_config_secret(stored, "", "agents.api_key", "agents.coder.api_key"), ConfigSecretProtectionError);
}

TEST(ConfigSecretProtectionTest, RejectsMalformedPayloadWithoutEchoingPayload) {
    try {
        (void)reveal_config_secret("enc:v1:not-valid-***", "password", "agent.api_key", "agent.api_key");
        FAIL() << "expected malformed payload to fail";
    } catch (const ConfigSecretProtectionError &e) {
        const std::string message = e.what();
        EXPECT_EQ(message.find("not-valid-***"), std::string::npos);
    }
}

TEST(ConfigSecretProtectionTest, FieldBindingPreventsReplayAcrossSecretKinds) {
    const auto stored = protect_config_secret("qq-secret", "shared-password", "qq.client_secret");
    EXPECT_THROW((void)reveal_config_secret(stored, "shared-password", "agents.api_key", "agents.default.api_key"), ConfigSecretProtectionError);
}

TEST_F(ConfigSecretFileTest, ProtectFlowEncryptsEligibleSecretsAndSkipsEnvironmentReferences) {
    write_config(R"toml(
[agent]
api_key = "plain-agent-key" # keep comment

[agents.coder]
api_key = "${CODER_KEY}"

[qq]
client_secret = "plain-qq-secret"

[memory]
mirror_file = "notes/MEMORY.md"
)toml");

    const auto result = protect_config_file_secrets(config_path(), "protect-password");
    ASSERT_TRUE(result.modified);
    EXPECT_EQ(result.protected_count, 2U);
    EXPECT_TRUE(std::filesystem::exists(result.backup_path));

    const auto updated = read_config(config_path());
    EXPECT_NE(updated.find("enc:v1:"), std::string::npos);
    EXPECT_NE(updated.find("api_key = \"${CODER_KEY}\""), std::string::npos);
    EXPECT_NE(updated.find("# keep comment"), std::string::npos);
    EXPECT_NE(updated.find("mirror_file = \"notes/MEMORY.md\""), std::string::npos);

    const auto backup = read_config(result.backup_path);
    EXPECT_NE(backup.find("plain-agent-key"), std::string::npos);
    EXPECT_NE(backup.find("plain-qq-secret"), std::string::npos);
}

TEST_F(ConfigSecretFileTest, ProtectFlowPreservesConfigPermissions) {
    write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

    std::filesystem::permissions(config_path(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
    const auto original_permissions = std::filesystem::status(config_path()).permissions();

    const auto result = protect_config_file_secrets(config_path(), "protect-password");
    ASSERT_TRUE(result.modified);
    EXPECT_EQ(std::filesystem::status(config_path()).permissions(), original_permissions);
}

TEST_F(ConfigSecretFileTest, ProtectFlowSkipsNestedAgentTables) {
    write_config(R"toml(
[agents.coder]
api_key = "plain-agent-key"

[agents.coder.extra]
api_key = "nested-agent-key"
)toml");

    const auto result = protect_config_file_secrets(config_path(), "protect-password");
    ASSERT_TRUE(result.modified);
    EXPECT_EQ(result.protected_count, 1U);

    const auto updated = read_config(config_path());
    EXPECT_NE(updated.find("[agents.coder]\napi_key = \"enc:v1:"), std::string::npos);
    EXPECT_NE(updated.find("[agents.coder.extra]\napi_key = \"nested-agent-key\""), std::string::npos);
}

TEST_F(ConfigSecretFileTest, ProtectFlowLeavesFileUntouchedOnFailure) {
    write_config(R"toml(
[agent]
api_key = "plain-agent-key"
)toml");

    const auto original = read_config(config_path());
    EXPECT_THROW((void)protect_config_file_secrets(config_path(), ""), ConfigSecretProtectionError);
    EXPECT_EQ(read_config(config_path()), original);
    EXPECT_FALSE(std::filesystem::exists(config_path().string() + ".bak"));
}

} // namespace
