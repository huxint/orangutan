#include "app/runtime/identity.hpp"
#include "app/runtime/memory-context.hpp"
#include "infra/config/config.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace orangutan;

class RuntimeIdentityTest : public ::testing::Test {
protected:
    void SetUp() override {
        workspace_root_ = std::filesystem::temp_directory_path() / "orangutan_identity_workspace_test";
        std::filesystem::remove_all(workspace_root_);
        std::filesystem::create_directories(workspace_root_);
    }

    void TearDown() override {
        std::filesystem::remove_all(workspace_root_);
    }

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

private:
    std::filesystem::path workspace_root_;
};

TEST_F(RuntimeIdentityTest, ResolveWorkspaceRootCanonicalizesExistingDirectory) {
    auto nested = workspace_root() / "nested";
    std::filesystem::create_directories(nested);

    auto resolved = resolve_workspace_root((workspace_root() / "." / "nested" / "..").string());

    EXPECT_EQ(resolved, std::filesystem::weakly_canonical(workspace_root()).string());
}

TEST_F(RuntimeIdentityTest, ResolveWorkspaceRootCreatesMissingDirectory) {
    auto missing = workspace_root() / "fresh-root";
    std::filesystem::remove_all(missing);

    auto resolved = resolve_workspace_root(missing.string());

    EXPECT_TRUE(std::filesystem::exists(resolved));
    EXPECT_TRUE(std::filesystem::is_directory(resolved));
    EXPECT_EQ(resolved, std::filesystem::weakly_canonical(missing).string());
}

TEST_F(RuntimeIdentityTest, ResolveWorkspaceRootDefaultsToMainWorkspaceWhenUnset) {
    auto home_root = workspace_root() / "home";
    std::filesystem::create_directories(home_root);
    orangutan::testing::ScopedEnvVar home_env("HOME", home_root.string());

    auto resolved = resolve_workspace_root("");
    const auto expected = std::filesystem::weakly_canonical(home_root / ".orangutan" / "workspace" / "main").string();

    EXPECT_EQ(resolved, expected);
    EXPECT_TRUE(std::filesystem::exists(expected));
    EXPECT_TRUE(std::filesystem::is_directory(expected));
}

TEST_F(RuntimeIdentityTest, ResolveWorkspaceRootRejectsMissingHomeForDefaultWorkspace) {
    orangutan::testing::ScopedEnvVar home_env("HOME", "");

    EXPECT_THROW(resolve_workspace_root(""), std::runtime_error);
}

TEST_F(RuntimeIdentityTest, ResolveWorkspaceRootRejectsRegularFile) {
    auto file_path = workspace_root() / "not-a-directory";
    std::ofstream(file_path) << "content";

    EXPECT_THROW(resolve_workspace_root(file_path.string()), std::runtime_error);
}

TEST_F(RuntimeIdentityTest, DeriveCliRuntimeKeyUsesLocalRuntimeForDefaultAgent) {
    EXPECT_EQ(derive_cli_runtime_key("default"), "cli:local");
}

TEST_F(RuntimeIdentityTest, DeriveCliSessionScopePreservesLegacyDefaultScope) {
    EXPECT_EQ(derive_cli_session_scope("default"), "");
}

TEST_F(RuntimeIdentityTest, DeriveCliSessionScopeUsesAgentScopeForNamedAgent) {
    EXPECT_EQ(derive_cli_session_scope("coder"), "agent:coder");
}

TEST_F(RuntimeIdentityTest, DeriveCliRuntimeKeyPrefixesNamedAgent) {
    EXPECT_EQ(derive_cli_runtime_key("coder"), "agent:coder|cli:local");
}

TEST_F(RuntimeIdentityTest, DeriveCliIdentityUsesResolvedWorkspaceAndNamedAgentScope) {
    const auto identity = derive_cli_identity(workspace_root().string(), "coder");

    EXPECT_EQ(identity.runtime_key, "agent:coder|cli:local");
    EXPECT_EQ(identity.memory_scope, "agent:coder");
    EXPECT_EQ(identity.workspace, std::filesystem::weakly_canonical(workspace_root()).string());
}

TEST_F(RuntimeIdentityTest, DeriveChannelIdentityCreatesStableScopedWorkspace) {
    const auto first = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "default");
    const auto second = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "default");

    EXPECT_EQ(first.runtime_key, "agent:default|jid:qqbot:c2c:alice");
    EXPECT_EQ(first.memory_scope, first.runtime_key);
    EXPECT_EQ(first.workspace, second.workspace);
    EXPECT_TRUE(std::filesystem::exists(first.workspace));
    EXPECT_TRUE(std::filesystem::is_directory(first.workspace));
    EXPECT_EQ(std::filesystem::path(first.workspace).parent_path(), std::filesystem::weakly_canonical(workspace_root()));
}

TEST_F(RuntimeIdentityTest, DifferentJidsUseDifferentWorkspaces) {
    const auto alice = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "default");
    const auto bob = derive_channel_identity(workspace_root().string(), "qqbot:c2c:bob", "default");

    EXPECT_NE(alice.workspace, bob.workspace);
    EXPECT_NE(alice.memory_scope, bob.memory_scope);
}

TEST_F(RuntimeIdentityTest, SameJidDifferentAgentsUseDifferentScopesAndWorkspaces) {
    const auto general = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "default");
    const auto coder = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "coder");

    EXPECT_NE(general.runtime_key, coder.runtime_key);
    EXPECT_NE(general.memory_scope, coder.memory_scope);
    EXPECT_NE(general.workspace, coder.workspace);
}

TEST_F(RuntimeIdentityTest, ParentPromptGuidanceMentionsStatusPollingTool) {
    const auto prompt = append_subagent_prompt_guidance("Parent base prompt.", {"coder"}, false);

    EXPECT_NE(prompt.find("Use `subagent_status`"), std::string::npos);
    EXPECT_NE(prompt.find("poll later with `subagent_status`"), std::string::npos);
}

TEST_F(RuntimeIdentityTest, MakeRuntimeMemoryContextResolvesWorkspaceRelativeMirrorPaths) {
    Config::MemoryConfig memory_cfg;
    memory_cfg.mirror_enabled = true;
    memory_cfg.mirror_file = "notes/MEMORY.md";
    memory_cfg.journal_dir = "journals";

    const auto identity = derive_channel_identity(workspace_root().string(), "qqbot:c2c:alice", "coder");
    const auto context = make_runtime_memory_context(identity, memory_cfg);

    EXPECT_EQ(context.scope, identity.memory_scope);
    EXPECT_EQ(context.workspace, identity.workspace);
    EXPECT_TRUE(context.mirror.enabled);
    EXPECT_EQ(context.snapshot_path(), std::filesystem::path(identity.workspace) / "notes" / "MEMORY.md");
    EXPECT_EQ(context.journal_dir(), std::filesystem::path(identity.workspace) / "journals");
}

TEST(RuntimeMemoryContextTest, MakeRuntimeMemoryContextLeavesPathsEmptyWithoutWorkspace) {
    RuntimeIdentity identity{
        .workspace = {},
        .runtime_key = "agent:coder|cli:local",
        .memory_scope = "agent:coder",
    };
    Config::MemoryConfig memory_cfg;
    memory_cfg.mirror_enabled = true;

    const auto context = make_runtime_memory_context(identity, memory_cfg);
    EXPECT_TRUE(context.mirror.enabled);
    EXPECT_TRUE(context.snapshot_path().empty());
    EXPECT_TRUE(context.journal_dir().empty());
}
