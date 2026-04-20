#include "bootstrap/identity.hpp"
#include "bootstrap/memory-context.hpp"
#include "config/config.hpp"
#include "orchestration/types.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>

namespace {

    namespace bootstrap = orangutan::bootstrap;
    namespace orchestration = orangutan::orchestration;

    TEST_CASE("resolve_workspace_root_canonicalizes_existing_directory") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-workspace");
        const auto nested = workspace_root / "nested";
        std::filesystem::create_directories(nested);

        const auto resolved = bootstrap::resolve_workspace_root((workspace_root / "." / "nested" / "..").string());

        CHECK(resolved == std::filesystem::weakly_canonical(workspace_root).string());
    };

    TEST_CASE("resolve_workspace_root_creates_missing_directory") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-missing-workspace");
        const auto missing = workspace_root / "fresh-root";
        std::filesystem::remove_all(missing);

        const auto resolved = bootstrap::resolve_workspace_root(missing.string());

        CHECK(std::filesystem::exists(resolved));
        CHECK(std::filesystem::is_directory(resolved));
        CHECK(resolved == std::filesystem::weakly_canonical(missing).string());
    };

    TEST_CASE("resolve_workspace_root_defaults_to_home_workspace_when_unset") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-default-workspace");
        const auto home_root = workspace_root / "home";
        std::filesystem::create_directories(home_root);
        orangutan::testing::ScopedEnvVar home_env("HOME", home_root.string());

        const auto resolved = bootstrap::resolve_workspace_root("");
        const auto expected = std::filesystem::weakly_canonical(home_root / ".orangutan" / "workspace" / "main").string();

        CHECK(resolved == expected);
        CHECK(std::filesystem::exists(expected));
        CHECK(std::filesystem::is_directory(expected));
    };

    TEST_CASE("resolve_workspace_root_rejects_missing_home_for_default_workspace") {
        orangutan::testing::ScopedEnvVar home_env("HOME", "");
        REQUIRE_THROWS_AS(bootstrap::resolve_workspace_root(""), std::runtime_error);
    };

    TEST_CASE("resolve_workspace_root_rejects_regular_file") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-regular-file");
        const auto file_path = workspace_root / "not-a-directory";
        std::ofstream(file_path) << "content";

        REQUIRE_THROWS_AS(bootstrap::resolve_workspace_root(file_path.string()), std::runtime_error);
    };

    TEST_CASE("derive_cli_runtime_key_uses_local_runtime_for_default_agent") {
        CHECK(bootstrap::derive_cli_runtime_key("default") == "cli:local");
    };

    TEST_CASE("derive_cli_session_scope_preserves_legacy_default_scope") {
        CHECK(bootstrap::derive_cli_session_scope("default").empty());
    };

    TEST_CASE("derive_cli_session_scope_uses_agent_scope_for_named_agent") {
        CHECK(bootstrap::derive_cli_session_scope("coder") == "agent:coder");
    };

    TEST_CASE("derive_cli_runtime_key_prefixes_named_agent") {
        CHECK(bootstrap::derive_cli_runtime_key("coder") == "agent:coder|cli:local");
    };

    TEST_CASE("derive_cli_identity_uses_resolved_workspace_and_named_agent_scope") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-cli");
        const auto identity = bootstrap::derive_cli_identity(workspace_root.string(), "coder");

        CHECK(identity.runtime_key == "agent:coder|cli:local");
        CHECK(identity.memory_scope == "agent:coder");
        CHECK(identity.workspace == std::filesystem::weakly_canonical(workspace_root).string());
    };

    TEST_CASE("derive_channel_identity_keeps_workspace_root_and_scoped_memory") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-channel");
        const auto first = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto second = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");

        CHECK(first.runtime_key == "agent:default|jid:qqbot:c2c:alice");
        CHECK(first.memory_scope == first.runtime_key);
        CHECK(first.workspace == second.workspace);
        CHECK(first.workspace == std::filesystem::weakly_canonical(workspace_root).string());
    };

    TEST_CASE("different_jids_use_different_memory_scopes") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-different-jids");
        const auto alice = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto bob = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:bob", "default");

        CHECK(alice.workspace == bob.workspace);
        CHECK(alice.memory_scope != bob.memory_scope);
    };

    TEST_CASE("same_jid_different_agents_use_different_scopes_and_shared_workspace_root") {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-different-agents");
        const auto general = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto coder = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "coder");

        CHECK(general.runtime_key != coder.runtime_key);
        CHECK(general.memory_scope != coder.memory_scope);
        CHECK(general.workspace == coder.workspace);
    };

    TEST_CASE("parent_prompt_guidance_mentions_agent_spawn_tool") {
        const auto prompt = bootstrap::append_agent_prompt_guidance("Parent base prompt.", {"coder"}, orchestration::agent_role::standalone);

        CHECK(prompt.contains("`agent_spawn`"));
        CHECK(prompt.contains("Avoid spawning agents for trivial tasks"));
    };

    TEST_CASE("make_runtime_memory_context_resolves_workspace_relative_mirror_paths") {
        orangutan::Config::MemoryConfig memory_cfg;
        memory_cfg.mirror_enabled = true;
        memory_cfg.mirror_file = "notes/MEMORY.md";
        memory_cfg.journal_dir = "journals";

        const auto workspace_root = orangutan::testing::unique_test_root("identity-memory-context");
        const auto identity = bootstrap::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "coder");
        const auto context = bootstrap::make_runtime_memory_context(identity, memory_cfg);

        CHECK(context.scope == identity.memory_scope);
        CHECK(context.workspace == identity.workspace);
        CHECK(context.mirror.enabled);
        CHECK(context.snapshot_path() == std::filesystem::path(identity.workspace) / "notes" / "MEMORY.md");
        CHECK(context.journal_dir() == std::filesystem::path(identity.workspace) / "journals");
    };

    TEST_CASE("make_runtime_memory_context_leaves_paths_empty_without_workspace") {
        bootstrap::RuntimeIdentity identity{
            .workspace = {},
            .runtime_key = "agent:coder|cli:local",
            .memory_scope = "agent:coder",
        };
        orangutan::Config::MemoryConfig memory_cfg;
        memory_cfg.mirror_enabled = true;

        const auto context = bootstrap::make_runtime_memory_context(identity, memory_cfg);
        CHECK(context.mirror.enabled);
        CHECK(context.snapshot_path().empty());
        CHECK(context.journal_dir().empty());
    };

} // namespace
