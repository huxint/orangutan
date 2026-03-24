#include "app/runtime/identity.hpp"
#include "app/runtime/memory-context.hpp"
#include "infra/config/config.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <fstream>
#include "support/ut.hpp"

namespace {

boost::ut::suite runtime_identity_suite = [] {
    using namespace boost::ut;

    "resolve_workspace_root_canonicalizes_existing_directory"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-workspace");
        const auto nested = workspace_root / "nested";
        std::filesystem::create_directories(nested);

        const auto resolved = orangutan::resolve_workspace_root((workspace_root / "." / "nested" / "..").string());

        expect(resolved == std::filesystem::weakly_canonical(workspace_root).string());
    };

    "resolve_workspace_root_creates_missing_directory"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-missing-workspace");
        const auto missing = workspace_root / "fresh-root";
        std::filesystem::remove_all(missing);

        const auto resolved = orangutan::resolve_workspace_root(missing.string());

        expect(std::filesystem::exists(resolved));
        expect(std::filesystem::is_directory(resolved));
        expect(resolved == std::filesystem::weakly_canonical(missing).string());
    };

    "resolve_workspace_root_defaults_to_main_workspace_when_unset"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-default-workspace");
        const auto home_root = workspace_root / "home";
        std::filesystem::create_directories(home_root);
        orangutan::testing::ScopedEnvVar home_env("HOME", home_root.string());

        const auto resolved = orangutan::resolve_workspace_root("");
        const auto expected = std::filesystem::weakly_canonical(home_root / ".orangutan" / "workspace" / "main").string();

        expect(resolved == expected);
        expect(std::filesystem::exists(expected));
        expect(std::filesystem::is_directory(expected));
    };

    "resolve_workspace_root_rejects_missing_home_for_default_workspace"_test = [] {
        orangutan::testing::ScopedEnvVar home_env("HOME", "");
        expect(throws<std::runtime_error>([] {
            static_cast<void>(orangutan::resolve_workspace_root(""));
        }));
    };

    "resolve_workspace_root_rejects_regular_file"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-regular-file");
        const auto file_path = workspace_root / "not-a-directory";
        std::ofstream(file_path) << "content";

        expect(throws<std::runtime_error>([&] {
            static_cast<void>(orangutan::resolve_workspace_root(file_path.string()));
        }));
    };

    "derive_cli_runtime_key_uses_local_runtime_for_default_agent"_test = [] {
        expect(orangutan::derive_cli_runtime_key("default") == "cli:local");
    };

    "derive_cli_session_scope_preserves_legacy_default_scope"_test = [] {
        expect(orangutan::derive_cli_session_scope("default").empty());
    };

    "derive_cli_session_scope_uses_agent_scope_for_named_agent"_test = [] {
        expect(orangutan::derive_cli_session_scope("coder") == "agent:coder");
    };

    "derive_cli_runtime_key_prefixes_named_agent"_test = [] {
        expect(orangutan::derive_cli_runtime_key("coder") == "agent:coder|cli:local");
    };

    "derive_cli_identity_uses_resolved_workspace_and_named_agent_scope"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-cli");
        const auto identity = orangutan::derive_cli_identity(workspace_root.string(), "coder");

        expect(identity.runtime_key == "agent:coder|cli:local");
        expect(identity.memory_scope == "agent:coder");
        expect(identity.workspace == std::filesystem::weakly_canonical(workspace_root).string());
    };

    "derive_channel_identity_creates_stable_scoped_workspace"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-channel");
        const auto first = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto second = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");

        expect(first.runtime_key == "agent:default|jid:qqbot:c2c:alice");
        expect(first.memory_scope == first.runtime_key);
        expect(first.workspace == second.workspace);
        expect(std::filesystem::exists(first.workspace));
        expect(std::filesystem::is_directory(first.workspace));
        expect(std::filesystem::path(first.workspace).parent_path() == std::filesystem::weakly_canonical(workspace_root));
    };

    "different_jids_use_different_workspaces"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-different-jids");
        const auto alice = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto bob = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:bob", "default");

        expect(alice.workspace != bob.workspace);
        expect(alice.memory_scope != bob.memory_scope);
    };

    "same_jid_different_agents_use_different_scopes_and_workspaces"_test = [] {
        const auto workspace_root = orangutan::testing::unique_test_root("identity-different-agents");
        const auto general = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "default");
        const auto coder = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "coder");

        expect(general.runtime_key != coder.runtime_key);
        expect(general.memory_scope != coder.memory_scope);
        expect(general.workspace != coder.workspace);
    };

    "parent_prompt_guidance_mentions_status_polling_tool"_test = [] {
        const auto prompt = orangutan::append_subagent_prompt_guidance("Parent base prompt.", {"coder"}, false);

        expect(prompt.find("Use `subagent_status`") != std::string::npos);
        expect(prompt.find("poll later with `subagent_status`") != std::string::npos);
    };

    "make_runtime_memory_context_resolves_workspace_relative_mirror_paths"_test = [] {
        orangutan::Config::MemoryConfig memory_cfg;
        memory_cfg.mirror_enabled = true;
        memory_cfg.mirror_file = "notes/MEMORY.md";
        memory_cfg.journal_dir = "journals";

        const auto workspace_root = orangutan::testing::unique_test_root("identity-memory-context");
        const auto identity = orangutan::derive_channel_identity(workspace_root.string(), "qqbot:c2c:alice", "coder");
        const auto context = orangutan::make_runtime_memory_context(identity, memory_cfg);

        expect(context.scope == identity.memory_scope);
        expect(context.workspace == identity.workspace);
        expect(context.mirror.enabled);
        expect(context.snapshot_path() == std::filesystem::path(identity.workspace) / "notes" / "MEMORY.md");
        expect(context.journal_dir() == std::filesystem::path(identity.workspace) / "journals");
    };
};

boost::ut::suite runtime_memory_context_suite = [] {
    using namespace boost::ut;

    "make_runtime_memory_context_leaves_paths_empty_without_workspace"_test = [] {
        orangutan::RuntimeIdentity identity{
            .workspace = {},
            .runtime_key = "agent:coder|cli:local",
            .memory_scope = "agent:coder",
        };
        orangutan::Config::MemoryConfig memory_cfg;
        memory_cfg.mirror_enabled = true;

        const auto context = orangutan::make_runtime_memory_context(identity, memory_cfg);
        expect(context.mirror.enabled);
        expect(context.snapshot_path().empty());
        expect(context.journal_dir().empty());
    };
};

} // namespace
