#include "infra/config/config.hpp"
#include "test-helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

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
        std::string write_config(const std::string &content) const {
            const auto path = (tmp_dir_ / "config.toml").string();
            std::ofstream ofs(path);
            ofs << content;
            return path;
        }

    private:
        std::filesystem::path tmp_dir_;
    };

    TEST_CASE("default_values") {
        const Config cfg;
        CHECK(cfg.model == "claude-sonnet-4-20250514");
        CHECK(cfg.base_url == "https://api.anthropic.com");
        CHECK(cfg.temperature == 1.0);
        CHECK(cfg.max_iterations == 20);
        CHECK(cfg.max_tokens == 4096);
        CHECK(cfg.workspace.empty());
        CHECK(cfg.allowed_tools.empty());
        CHECK(cfg.denied_tools.empty());
        CHECK(cfg.permissions.sandbox_mode == ToolSandboxMode::isolated);
        CHECK(cfg.permissions.shell_approval == ToolApprovalPolicy::ask);
        CHECK(cfg.permissions.denied_shell_commands.empty());
        CHECK(cfg.auto_save);
        CHECK_FALSE(cfg.memory.mirror_enabled);
        CHECK(cfg.memory.mirror_file == "MEMORY.md");
        CHECK(cfg.memory.journal_dir == "memory");
    };

    TEST_CASE("default_edit_mode_is_hashline") {
        const Config cfg;
        CHECK(cfg.edit_mode == "hashline");
    };

    TEST_CASE("parses_agent_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "claude-opus-4-20250514"
base_url = "http://localhost:8080"
temperature = 0.5
max_iterations = 10
max_tokens = 8192
workspace = "/tmp/orangutan-workspace"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.model == "claude-opus-4-20250514");
        CHECK(cfg.base_url == "http://localhost:8080");
        CHECK(cfg.temperature == 0.5);
        CHECK(cfg.max_iterations == 10);
        CHECK(cfg.max_tokens == 8192);
        CHECK(cfg.workspace == "/tmp/orangutan-workspace");
    };

    TEST_CASE("parses_fallback_models_with_inheritance_and_overrides") {
        ConfigFileHarness harness;
        ScopedEnvVar fallback_env("ORANGUTAN_TEST_FALLBACK", "global-fallback-b");
        const auto path = harness.write_config(R"toml(
[agent]
model = "primary-model"
fallback_models = ["global-fallback-a", "${ORANGUTAN_TEST_FALLBACK}"]

[agents.default]
model = "default-model"

[agents.coder]
model = "coder-model"
fallback_models = ["coder-fallback-a", "coder-fallback-b"]
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.fallback_models.size() == 2ul);
        CHECK(cfg.fallback_models[0] == "global-fallback-a");
        CHECK(cfg.fallback_models[1] == "global-fallback-b");

        REQUIRE(cfg.agents.contains("default"));
        REQUIRE(cfg.agents.contains("coder"));
        CHECK(cfg.agents.at("default").fallback_models.size() == 2ul);
        CHECK(cfg.agents.at("default").fallback_models[0] == "global-fallback-a");
        CHECK(cfg.agents.at("default").fallback_models[1] == "global-fallback-b");
        CHECK(cfg.agents.at("coder").fallback_models.size() == 2ul);
        CHECK(cfg.agents.at("coder").fallback_models[0] == "coder-fallback-a");
        CHECK(cfg.agents.at("coder").fallback_models[1] == "coder-fallback-b");
    };

    TEST_CASE("parses_tools_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[tools]
allowed = ["read", "write", "ls"]
denied = ["shell"]
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.allowed_tools.size() == 3ul);
        CHECK(cfg.allowed_tools[0] == "read");
        CHECK(cfg.allowed_tools[1] == "write");
        CHECK(cfg.allowed_tools[2] == "ls");
        CHECK(cfg.denied_tools.size() == 1ul);
        CHECK(cfg.denied_tools[0] == "shell");
        CHECK(cfg.permissions.allowed_tools.size() == 3ul);
        CHECK(cfg.permissions.allowed_tools[0] == "read");
        CHECK(cfg.permissions.allowed_tools[1] == "write");
        CHECK(cfg.permissions.allowed_tools[2] == "ls");
        CHECK(cfg.permissions.denied_tools.size() == 1ul);
        CHECK(cfg.permissions.denied_tools[0] == "shell");
    };

    TEST_CASE("parses_permissions_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[permissions]
sandbox_mode = "workspace-write"
shell_approval_policy = "deny"
allowed_tools = ["read", "write"]
denied_tools = ["shell"]
denied_shell_commands = ["rm -rf", "shutdown"]
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.permissions.sandbox_mode == ToolSandboxMode::workspace_write);
        CHECK(cfg.permissions.shell_approval == ToolApprovalPolicy::deny);
        CHECK(cfg.permissions.allowed_tools.size() == 2ul);
        CHECK(cfg.permissions.allowed_tools[0] == "read");
        CHECK(cfg.permissions.allowed_tools[1] == "write");
        CHECK(cfg.permissions.denied_tools.size() == 1ul);
        CHECK(cfg.permissions.denied_tools[0] == "shell");
        CHECK(cfg.permissions.denied_shell_commands.size() == 2ul);
        CHECK(cfg.permissions.denied_shell_commands[0] == "rm -rf");
        CHECK(cfg.permissions.denied_shell_commands[1] == "shutdown");
    };

    TEST_CASE("parses_permission_enum_aliases") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[permissions]
sandbox_mode = "workspace_write"
shell_approval_policy = "ALLOW"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.permissions.sandbox_mode == ToolSandboxMode::workspace_write);
        CHECK(cfg.permissions.shell_approval == ToolApprovalPolicy::allow);
    };

    TEST_CASE("parses_custom_tools_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[[tools.custom]]
name = "ls"
description = "List files"
command = "ls -la ${path}"
timeout = 15
working_dir = "~/workspace"

[tools.custom.input_schema]
path = "string"

[[tools.custom]]
name = "grep"
description = "Search files"
command = "rg --color=never -n ${pattern} ${path}"

[tools.custom.input_schema]
pattern = "string"
path = "string"
)toml");

        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto cfg = Config::load_from(path);
        CHECK(cfg.custom_tools.size() == 2ul);
        CHECK(cfg.custom_tools[0].name == "ls");
        CHECK(cfg.custom_tools[0].command == "ls -la ${path}");
        CHECK(cfg.custom_tools[0].timeout == 15);
        CHECK(cfg.custom_tools[0].working_dir == std::string(home) + "/workspace");
        CHECK(cfg.custom_tools[0].input_schema.at("path") == "string");
        CHECK(cfg.custom_tools[1].name == "grep");
        CHECK(cfg.custom_tools[1].command == "rg --color=never -n ${pattern} ${path}");
        CHECK(cfg.custom_tools[1].input_schema.at("pattern") == "string");
        CHECK(cfg.custom_tools[1].input_schema.at("path") == "string");
    };

    TEST_CASE("partial_config_keeps_defaults") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.model == "gpt-4");
        CHECK(cfg.base_url == "https://api.anthropic.com");
        CHECK(cfg.temperature == 1.0);
        CHECK(cfg.max_iterations == 20);
    };

    TEST_CASE("empty_file_returns_defaults") {
        ConfigFileHarness harness;
        const auto path = harness.write_config("");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.model == "claude-sonnet-4-20250514");
    };

    TEST_CASE("invalid_toml_returns_defaults") {
        ConfigFileHarness harness;
        const auto path = harness.write_config("this is [not valid toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.model == "claude-sonnet-4-20250514");
    };

    TEST_CASE("missing_file_returns_defaults") {
        const auto cfg = Config::load_from("/tmp/orangutan_nonexistent_config_xyz.toml");
        CHECK(cfg.model == "claude-sonnet-4-20250514");
    };

    TEST_CASE("parses_session_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[session]
auto_save = true
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.auto_save);
    };

    TEST_CASE("parses_memory_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[memory]
mirror_enabled = true
mirror_file = "notes/MEMORY.md"
journal_dir = "journals"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.memory.mirror_enabled);
        CHECK(cfg.memory.mirror_file == "notes/MEMORY.md");
        CHECK(cfg.memory.journal_dir == "journals");
    };

    TEST_CASE("parses_qq_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[qq]
app_id = "app-123"
client_secret = "secret-456"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.qq_app_id == "app-123");
        CHECK(cfg.qq_client_secret == "secret-456");
        CHECK(cfg.qq_bots.size() == 1ul);
        CHECK(cfg.qq_bots[0].name.empty());
        CHECK(cfg.qq_bots[0].app_id == "app-123");
        CHECK(cfg.qq_bots[0].client_secret == "secret-456");
        CHECK(cfg.qq_bots[0].agent == "default");
    };

    TEST_CASE("parses_named_agents_and_qq_bots") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
api_key = "legacy-default"

[agents.default]
model = "default-model"
workspace = "~/workspace/default"
subagents = ["coder"]

[agents.coder]
provider = "openai"
model = "coder-model"
base_url = "http://localhost:11434/v1"
api_key = "coder-key"
workspace = "~/workspace/coder"
subagents = []

[[qq_bots]]
name = "primary"
app_id = "app-a"
client_secret = "secret-a"
agent = "default"

[[qq_bots]]
name = "coder-bot"
app_id = "app-b"
client_secret = "secret-b"
agent = "coder"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.agents.size() == 2ul);
        REQUIRE(cfg.agents.contains("default"));
        REQUIRE(cfg.agents.contains("coder"));
        CHECK(cfg.agents.at("default").model == "default-model");
        CHECK(cfg.agents.at("default").subagents[0] == "coder");
        CHECK(cfg.agents.at("coder").provider == "openai");
        CHECK(cfg.agents.at("coder").api_key == "coder-key");
        CHECK(cfg.qq_bots.size() == 2ul);
        CHECK(cfg.qq_bots[0].name == "primary");
        CHECK(cfg.qq_bots[0].agent == "default");
        CHECK(cfg.qq_bots[1].name == "coder-bot");
        CHECK(cfg.qq_bots[1].agent == "coder");
    };

    TEST_CASE("agent_permissions_inherit_global_defaults_and_allow_overrides") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[permissions]
sandbox_mode = "isolated"
shell_approval_policy = "ask"
allowed_tools = ["read", "write"]
denied_shell_commands = ["rm -rf"]

[agents.default]
model = "default-model"

[agents.default.permissions]
shell_approval_policy = "allow"
allowed_tools = ["read"]

[agents.coder]
model = "coder-model"

[agents.coder.permissions]
sandbox_mode = "workspace-write"
denied_tools = ["shell"]
denied_shell_commands = ["curl"]
)toml");

        const auto cfg = Config::load_from(path);
        REQUIRE(cfg.agents.contains("default"));
        REQUIRE(cfg.agents.contains("coder"));

        const auto &default_agent = cfg.agents.at("default");
        CHECK(default_agent.permissions.sandbox_mode == ToolSandboxMode::isolated);
        CHECK(default_agent.permissions.shell_approval == ToolApprovalPolicy::allow);
        CHECK(default_agent.permissions.allowed_tools.size() == 1ul);
        CHECK(default_agent.permissions.allowed_tools[0] == "read");
        CHECK(default_agent.permissions.denied_shell_commands.size() == 1ul);
        CHECK(default_agent.permissions.denied_shell_commands[0] == "rm -rf");

        const auto &coder_agent = cfg.agents.at("coder");
        CHECK(coder_agent.permissions.sandbox_mode == ToolSandboxMode::workspace_write);
        CHECK(coder_agent.permissions.shell_approval == ToolApprovalPolicy::ask);
        CHECK(coder_agent.permissions.allowed_tools.size() == 2ul);
        CHECK(coder_agent.permissions.allowed_tools[0] == "read");
        CHECK(coder_agent.permissions.allowed_tools[1] == "write");
        CHECK(coder_agent.permissions.denied_tools.size() == 1ul);
        CHECK(coder_agent.permissions.denied_tools[0] == "shell");
        CHECK(coder_agent.permissions.denied_shell_commands.size() == 1ul);
        CHECK(coder_agent.permissions.denied_shell_commands[0] == "curl");
    };

    TEST_CASE("parses_security_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[security]
allow = ["cli:*", "qqbot:c2c:*"]
deny = ["qqbot:c2c:blocked"]
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.allow.size() == 2ul);
        CHECK(cfg.allow[0] == "cli:*");
        CHECK(cfg.allow[1] == "qqbot:c2c:*");
        CHECK(cfg.deny.size() == 1ul);
        CHECK(cfg.deny[0] == "qqbot:c2c:blocked");
    };

    TEST_CASE("parses_mcp_servers_section") {
        ConfigFileHarness harness;
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        const auto path = harness.write_config(R"toml(
[[mcp.servers]]
name = "github"
command = "~/bin/mock-mcp"
args = ["--token", "${HOME}/token.txt"]
timeout = 12

[mcp.servers.env]
GITHUB_TOKEN = "gh-test"
ROOT = "${HOME}/workspace"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.mcp_servers.size() == 1ul);
        const auto &server = cfg.mcp_servers[0];
        CHECK(server.name == "github");
        CHECK(server.command == std::string(home) + "/bin/mock-mcp");
        CHECK(server.args.size() == 2ul);
        CHECK(server.args[0] == "--token");
        CHECK(server.args[1] == std::string(home) + "/token.txt");
        CHECK(server.timeout == 12);
        CHECK(server.env.at("GITHUB_TOKEN") == "gh-test");
        CHECK(server.env.at("ROOT") == std::string(home) + "/workspace");
    };

    TEST_CASE("skips_invalid_mcp_servers") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[[mcp.servers]]
command = "missing-name"

[[mcp.servers]]
name = "missing-command"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.mcp_servers.empty());
    };

    TEST_CASE("later_mcp_server_overrides_earlier_duplicate_name") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[[mcp.servers]]
name = "github"
command = "old-command"
timeout = 3

[[mcp.servers]]
name = "github"
command = "new-command"
timeout = 9
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.mcp_servers.size() == 1ul);
        CHECK(cfg.mcp_servers[0].name == "github");
        CHECK(cfg.mcp_servers[0].command == "new-command");
        CHECK(cfg.mcp_servers[0].timeout == 9);
    };

    TEST_CASE("parses_api_key") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
api_key = "sk-test-key-12345"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.api_key == "sk-test-key-12345");
    };

    TEST_CASE("api_key_with_env_var_expansion") {
        ConfigFileHarness harness;
        ScopedEnvVar api_key_env("ORANGUTAN_TEST_KEY", "sk-from-env");
        const auto path = harness.write_config(R"toml(
[agent]
api_key = "${ORANGUTAN_TEST_KEY}"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.api_key == "sk-from-env");
    };

    TEST_CASE("env_var_expansion_in_base_url") {
        ConfigFileHarness harness;
        ScopedEnvVar host_env("ORANGUTAN_TEST_HOST", "myhost");
        const auto path = harness.write_config(R"toml(
[agent]
base_url = "http://${ORANGUTAN_TEST_HOST}:8080"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.base_url == "http://myhost:8080");
    };

    TEST_CASE("env_var_expansion_in_workspace") {
        ConfigFileHarness harness;
        ScopedEnvVar workspace_env("ORANGUTAN_TEST_WORKSPACE", "/tmp/orangutan-workspace-from-env");
        const auto path = harness.write_config(R"toml(
[agent]
workspace = "${ORANGUTAN_TEST_WORKSPACE}"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.workspace == "/tmp/orangutan-workspace-from-env");
    };

    TEST_CASE("tilde_expansion_in_workspace") {
        ConfigFileHarness harness;
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);
        const auto path = harness.write_config(R"toml(
[agent]
workspace = "~/projects/orangutan"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.workspace == std::string(home) + "/projects/orangutan");
    };

    TEST_CASE("default_api_key_is_empty") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.api_key.empty());
    };

    TEST_CASE("loads_protected_legacy_agent_api_key_with_explicit_password") {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-protected-legacy", "legacy-password", "agent.api_key");
        const auto path = harness.write_config("[agent]\napi_key = \"" + protected_key + "\"\n");

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "legacy-password",
                                                 });
        CHECK(cfg.api_key == "sk-protected-legacy");
        REQUIRE(cfg.agents.contains("default"));
        CHECK(cfg.agents.at("default").api_key == "sk-protected-legacy");
    };

    TEST_CASE("loads_protected_configured_agent_api_key_from_environment_password") {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-protected-agent", "env-password", "agents.api_key");
        ScopedEnvVar config_password("ORANGUTAN_CONFIG_PASSWORD", "env-password");
        const auto path = harness.write_config("[agents.default]\napi_key = \"" + protected_key + "\"\n");

        const auto cfg = Config::load_from(path);
        REQUIRE(cfg.agents.contains("default"));
        CHECK(cfg.agents.at("default").api_key == "sk-protected-agent");
    };

    TEST_CASE("named_agents_can_inherit_protected_legacy_api_key") {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-inherited-agent", "inherit-password", "agent.api_key");
        const auto path = harness.write_config(
            R"toml(
[agent]
api_key = ")toml" +
            protected_key +
            R"toml("
model = "legacy-model"

[agents.coder]
model = "coder-model"
)toml");

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "inherit-password",
                                                 });
        REQUIRE(cfg.agents.contains("coder"));
        CHECK(cfg.agents.at("coder").api_key == "sk-inherited-agent");
    };

    TEST_CASE("loads_protected_qq_client_secret_with_explicit_password") {
        ConfigFileHarness harness;
        const auto protected_secret = protect_config_secret("qq-secret-value", "qq-password", "qq.client_secret");
        const auto path = harness.write_config(
            R"toml(
[agents.default]
model = "test-model"
api_key = "test-key"

[qq]
app_id = "qq-app"
)toml" + std::string("client_secret = \"") +
            protected_secret + "\"\n");

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "qq-password",
                                                 });
        CHECK(cfg.qq_bots.size() == 1ul);
        CHECK(cfg.qq_bots[0].client_secret == "qq-secret-value");
    };

    TEST_CASE("throws_when_protected_secret_has_no_password_source") {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-no-password", "expected-password", "agent.api_key");
        const auto path = harness.write_config("[agent]\napi_key = \"" + protected_key + "\"\n");

        REQUIRE_THROWS_AS(Config::load_from(path), ConfigSecretProtectionError);
    };

    TEST_CASE("parses_edit_mode_from_tools_section") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[tools]
edit_mode = "search_replace"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.edit_mode == "search_replace");
    };

    TEST_CASE("edit_mode_defaults_to_hashline_when_not_specified") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "test-model"
)toml");

        const auto cfg = Config::load_from(path);
        CHECK(cfg.edit_mode == "hashline");
    };

    TEST_CASE("agents_inherit_global_edit_mode_and_can_override_it") {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[tools]
edit_mode = "search_replace"

[agents.default]
model = "default-model"

[agents.coder]
model = "coder-model"
edit_mode = "hashline"
)toml");

        const auto cfg = Config::load_from(path);
        REQUIRE(cfg.agents.contains("default"));
        REQUIRE(cfg.agents.contains("coder"));
        CHECK(cfg.agents.at("default").edit_mode == "search_replace");
        CHECK(cfg.agents.at("coder").edit_mode == "hashline");
    };

    TEST_CASE("empty_lists_allow_all") {
        const Config cfg;
        CHECK(cfg.is_tool_allowed("shell"));
        CHECK(cfg.is_tool_allowed("read"));
        CHECK(cfg.is_tool_allowed("anything"));
    };

    TEST_CASE("denied_tool_is_blocked") {
        Config cfg;
        cfg.denied_tools = {"shell"};
        CHECK_FALSE(cfg.is_tool_allowed("shell"));
        CHECK(cfg.is_tool_allowed("read"));
    };

    TEST_CASE("allowed_list_restricts_tools") {
        Config cfg;
        cfg.allowed_tools = {"read", "ls"};
        CHECK(cfg.is_tool_allowed("read"));
        CHECK(cfg.is_tool_allowed("ls"));
        CHECK_FALSE(cfg.is_tool_allowed("shell"));
    };

    TEST_CASE("denied_takes_precedence_over_allowed") {
        Config cfg;
        cfg.allowed_tools = {"shell", "read"};
        cfg.denied_tools = {"shell"};
        CHECK_FALSE(cfg.is_tool_allowed("shell"));
        CHECK(cfg.is_tool_allowed("read"));
    };

    TEST_CASE("expand_env_vars_expands_defined_variable") {
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto result = expand_env_vars("${HOME}/config");
        CHECK(result == std::string(home) + "/config");
    };

    TEST_CASE("expand_env_vars_undefined_variable_becomes_empty") {
        const auto result = expand_env_vars("prefix_${ORANGUTAN_UNDEFINED_XYZ_12345}_suffix");
        CHECK(result == "prefix__suffix");
    };

    TEST_CASE("expand_env_vars_multiple_variables") {
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto result = expand_env_vars("${HOME}:${HOME}");
        CHECK(result == std::string(home) + ":" + std::string(home));
    };

    TEST_CASE("expand_env_vars_no_variables_passes_through") {
        const auto result = expand_env_vars("plain text");
        CHECK(result == "plain text");
    };

    TEST_CASE("expand_env_vars_empty_input_returns_empty") {
        const auto result = expand_env_vars("");
        CHECK(result.empty());
    };

    TEST_CASE("expand_env_vars_unclosed_brace_kept_literal") {
        const auto result = expand_env_vars("prefix${NOCLOSE");
        CHECK(result == "prefix${NOCLOSE");
    };

    TEST_CASE("expand_env_vars_adjacent_variables") {
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto result = expand_env_vars("${HOME}${HOME}");
        CHECK(result == std::string(home) + std::string(home));
    };

    TEST_CASE("expand_home_path_expands_bare_home") {
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto result = expand_home_path("~");
        CHECK(result == std::string(home));
    };

    TEST_CASE("expand_home_path_expands_home_prefix") {
        const char *home = std::getenv("HOME");
        REQUIRE(home != nullptr);

        const auto result = expand_home_path("~/projects/orangutan");
        CHECK(result == std::string(home) + "/projects/orangutan");
    };

    TEST_CASE("expand_home_path_leaves_plain_path_untouched") {
        const auto result = expand_home_path("/tmp/orangutan");
        CHECK(result == "/tmp/orangutan");
    };

} // namespace
