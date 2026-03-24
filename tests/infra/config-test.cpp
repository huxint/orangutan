#include "infra/config/config.hpp"
#include "test-helpers.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "support/ut.hpp"

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

boost::ut::suite config_default_suite = [] {
    using namespace boost::ut;

    "default_values"_test = [] {
        const Config cfg;
        expect(cfg.model == "claude-sonnet-4-20250514");
        expect(cfg.base_url == "https://api.anthropic.com");
        expect(cfg.temperature == 1.0_d);
        expect(cfg.max_iterations == 20_i);
        expect(cfg.max_tokens == 4096_i);
        expect(cfg.workspace.empty());
        expect(cfg.allowed_tools.empty());
        expect(cfg.denied_tools.empty());
        expect(cfg.permissions.sandbox_mode == ToolSandboxMode::isolated);
        expect(cfg.permissions.shell_approval == ToolApprovalPolicy::ask);
        expect(cfg.permissions.denied_shell_commands.empty());
        expect(cfg.auto_save);
        expect(not cfg.memory.mirror_enabled);
        expect(cfg.memory.mirror_file == "MEMORY.md");
        expect(cfg.memory.journal_dir == "memory");
    };

    "default_edit_mode_is_hashline"_test = [] {
        const Config cfg;
        expect(cfg.edit_mode == "hashline");
    };
};

boost::ut::suite config_file_suite = [] {
    using namespace boost::ut;

    "parses_agent_section"_test = [] {
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
        expect(cfg.model == "claude-opus-4-20250514");
        expect(cfg.base_url == "http://localhost:8080");
        expect(cfg.temperature == 0.5_d);
        expect(cfg.max_iterations == 10_i);
        expect(cfg.max_tokens == 8192_i);
        expect(cfg.workspace == "/tmp/orangutan-workspace");
    };

    "parses_fallback_models_with_inheritance_and_overrides"_test = [] {
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
        expect(cfg.fallback_models.size() == 2_ul);
        expect(cfg.fallback_models[0] == "global-fallback-a");
        expect(cfg.fallback_models[1] == "global-fallback-b");

        expect((cfg.agents.contains("default")) >> fatal);
        expect((cfg.agents.contains("coder")) >> fatal);
        expect(cfg.agents.at("default").fallback_models.size() == 2_ul);
        expect(cfg.agents.at("default").fallback_models[0] == "global-fallback-a");
        expect(cfg.agents.at("default").fallback_models[1] == "global-fallback-b");
        expect(cfg.agents.at("coder").fallback_models.size() == 2_ul);
        expect(cfg.agents.at("coder").fallback_models[0] == "coder-fallback-a");
        expect(cfg.agents.at("coder").fallback_models[1] == "coder-fallback-b");
    };

    "parses_tools_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[tools]
allowed = ["read", "write", "ls"]
denied = ["shell"]
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.allowed_tools.size() == 3_ul);
        expect(cfg.allowed_tools[0] == "read");
        expect(cfg.allowed_tools[1] == "write");
        expect(cfg.allowed_tools[2] == "ls");
        expect(cfg.denied_tools.size() == 1_ul);
        expect(cfg.denied_tools[0] == "shell");
        expect(cfg.permissions.allowed_tools.size() == 3_ul);
        expect(cfg.permissions.allowed_tools[0] == "read");
        expect(cfg.permissions.allowed_tools[1] == "write");
        expect(cfg.permissions.allowed_tools[2] == "ls");
        expect(cfg.permissions.denied_tools.size() == 1_ul);
        expect(cfg.permissions.denied_tools[0] == "shell");
    };

    "parses_permissions_section"_test = [] {
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
        expect(cfg.permissions.sandbox_mode == ToolSandboxMode::workspace_write);
        expect(cfg.permissions.shell_approval == ToolApprovalPolicy::deny);
        expect(cfg.permissions.allowed_tools.size() == 2_ul);
        expect(cfg.permissions.allowed_tools[0] == "read");
        expect(cfg.permissions.allowed_tools[1] == "write");
        expect(cfg.permissions.denied_tools.size() == 1_ul);
        expect(cfg.permissions.denied_tools[0] == "shell");
        expect(cfg.permissions.denied_shell_commands.size() == 2_ul);
        expect(cfg.permissions.denied_shell_commands[0] == "rm -rf");
        expect(cfg.permissions.denied_shell_commands[1] == "shutdown");
    };

    "parses_custom_tools_section"_test = [] {
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
        expect((home != nullptr) >> fatal);

        const auto cfg = Config::load_from(path);
        expect(cfg.custom_tools.size() == 2_ul);
        expect(cfg.custom_tools[0].name == "ls");
        expect(cfg.custom_tools[0].command == "ls -la ${path}");
        expect(cfg.custom_tools[0].timeout == 15_i);
        expect(cfg.custom_tools[0].working_dir == std::string(home) + "/workspace");
        expect(cfg.custom_tools[0].input_schema.at("path") == "string");
        expect(cfg.custom_tools[1].name == "grep");
        expect(cfg.custom_tools[1].command == "rg --color=never -n ${pattern} ${path}");
        expect(cfg.custom_tools[1].input_schema.at("pattern") == "string");
        expect(cfg.custom_tools[1].input_schema.at("path") == "string");
    };

    "partial_config_keeps_defaults"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.model == "gpt-4");
        expect(cfg.base_url == "https://api.anthropic.com");
        expect(cfg.temperature == 1.0_d);
        expect(cfg.max_iterations == 20_i);
    };

    "empty_file_returns_defaults"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config("");

        const auto cfg = Config::load_from(path);
        expect(cfg.model == "claude-sonnet-4-20250514");
    };

    "invalid_toml_returns_defaults"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config("this is [not valid toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.model == "claude-sonnet-4-20250514");
    };

    "missing_file_returns_defaults"_test = [] {
        const auto cfg = Config::load_from("/tmp/orangutan_nonexistent_config_xyz.toml");
        expect(cfg.model == "claude-sonnet-4-20250514");
    };

    "parses_session_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[session]
auto_save = true
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.auto_save);
    };

    "parses_memory_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[memory]
mirror_enabled = true
mirror_file = "notes/MEMORY.md"
journal_dir = "journals"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.memory.mirror_enabled);
        expect(cfg.memory.mirror_file == "notes/MEMORY.md");
        expect(cfg.memory.journal_dir == "journals");
    };

    "parses_qq_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[qq]
app_id = "app-123"
client_secret = "secret-456"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.qq_app_id == "app-123");
        expect(cfg.qq_client_secret == "secret-456");
        expect(cfg.qq_bots.size() == 1_ul);
        expect(cfg.qq_bots[0].name.empty());
        expect(cfg.qq_bots[0].app_id == "app-123");
        expect(cfg.qq_bots[0].client_secret == "secret-456");
        expect(cfg.qq_bots[0].agent == "default");
    };

    "parses_named_agents_and_qq_bots"_test = [] {
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
        expect(cfg.agents.size() == 2_ul);
        expect((cfg.agents.contains("default")) >> fatal);
        expect((cfg.agents.contains("coder")) >> fatal);
        expect(cfg.agents.at("default").model == "default-model");
        expect(cfg.agents.at("default").subagents[0] == "coder");
        expect(cfg.agents.at("coder").provider == "openai");
        expect(cfg.agents.at("coder").api_key == "coder-key");
        expect(cfg.qq_bots.size() == 2_ul);
        expect(cfg.qq_bots[0].name == "primary");
        expect(cfg.qq_bots[0].agent == "default");
        expect(cfg.qq_bots[1].name == "coder-bot");
        expect(cfg.qq_bots[1].agent == "coder");
    };

    "agent_permissions_inherit_global_defaults_and_allow_overrides"_test = [] {
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
        expect((cfg.agents.contains("default")) >> fatal);
        expect((cfg.agents.contains("coder")) >> fatal);

        const auto &default_agent = cfg.agents.at("default");
        expect(default_agent.permissions.sandbox_mode == ToolSandboxMode::isolated);
        expect(default_agent.permissions.shell_approval == ToolApprovalPolicy::allow);
        expect(default_agent.permissions.allowed_tools.size() == 1_ul);
        expect(default_agent.permissions.allowed_tools[0] == "read");
        expect(default_agent.permissions.denied_shell_commands.size() == 1_ul);
        expect(default_agent.permissions.denied_shell_commands[0] == "rm -rf");

        const auto &coder_agent = cfg.agents.at("coder");
        expect(coder_agent.permissions.sandbox_mode == ToolSandboxMode::workspace_write);
        expect(coder_agent.permissions.shell_approval == ToolApprovalPolicy::ask);
        expect(coder_agent.permissions.allowed_tools.size() == 2_ul);
        expect(coder_agent.permissions.allowed_tools[0] == "read");
        expect(coder_agent.permissions.allowed_tools[1] == "write");
        expect(coder_agent.permissions.denied_tools.size() == 1_ul);
        expect(coder_agent.permissions.denied_tools[0] == "shell");
        expect(coder_agent.permissions.denied_shell_commands.size() == 1_ul);
        expect(coder_agent.permissions.denied_shell_commands[0] == "curl");
    };

    "parses_security_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[security]
allow = ["cli:*", "qqbot:c2c:*"]
deny = ["qqbot:c2c:blocked"]
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.allow.size() == 2_ul);
        expect(cfg.allow[0] == "cli:*");
        expect(cfg.allow[1] == "qqbot:c2c:*");
        expect(cfg.deny.size() == 1_ul);
        expect(cfg.deny[0] == "qqbot:c2c:blocked");
    };

    "parses_mcp_servers_section"_test = [] {
        ConfigFileHarness harness;
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);
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
        expect(cfg.mcp_servers.size() == 1_ul);
        const auto &server = cfg.mcp_servers[0];
        expect(server.name == "github");
        expect(server.command == std::string(home) + "/bin/mock-mcp");
        expect(server.args.size() == 2_ul);
        expect(server.args[0] == "--token");
        expect(server.args[1] == std::string(home) + "/token.txt");
        expect(server.timeout == 12_i);
        expect(server.env.at("GITHUB_TOKEN") == "gh-test");
        expect(server.env.at("ROOT") == std::string(home) + "/workspace");
    };

    "skips_invalid_mcp_servers"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[[mcp.servers]]
command = "missing-name"

[[mcp.servers]]
name = "missing-command"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.mcp_servers.empty());
    };

    "later_mcp_server_overrides_earlier_duplicate_name"_test = [] {
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
        expect(cfg.mcp_servers.size() == 1_ul);
        expect(cfg.mcp_servers[0].name == "github");
        expect(cfg.mcp_servers[0].command == "new-command");
        expect(cfg.mcp_servers[0].timeout == 9_i);
    };

    "parses_api_key"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
api_key = "sk-test-key-12345"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.api_key == "sk-test-key-12345");
    };

    "api_key_with_env_var_expansion"_test = [] {
        ConfigFileHarness harness;
        ScopedEnvVar api_key_env("ORANGUTAN_TEST_KEY", "sk-from-env");
        const auto path = harness.write_config(R"toml(
[agent]
api_key = "${ORANGUTAN_TEST_KEY}"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.api_key == "sk-from-env");
    };

    "env_var_expansion_in_base_url"_test = [] {
        ConfigFileHarness harness;
        ScopedEnvVar host_env("ORANGUTAN_TEST_HOST", "myhost");
        const auto path = harness.write_config(R"toml(
[agent]
base_url = "http://${ORANGUTAN_TEST_HOST}:8080"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.base_url == "http://myhost:8080");
    };

    "env_var_expansion_in_workspace"_test = [] {
        ConfigFileHarness harness;
        ScopedEnvVar workspace_env("ORANGUTAN_TEST_WORKSPACE", "/tmp/orangutan-workspace-from-env");
        const auto path = harness.write_config(R"toml(
[agent]
workspace = "${ORANGUTAN_TEST_WORKSPACE}"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.workspace == "/tmp/orangutan-workspace-from-env");
    };

    "tilde_expansion_in_workspace"_test = [] {
        ConfigFileHarness harness;
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);
        const auto path = harness.write_config(R"toml(
[agent]
workspace = "~/projects/orangutan"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.workspace == std::string(home) + "/projects/orangutan");
    };

    "default_api_key_is_empty"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.api_key.empty());
    };

    "loads_protected_legacy_agent_api_key_with_explicit_password"_test = [] {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-protected-legacy", "legacy-password", "agent.api_key");
        const auto path = harness.write_config("[agent]\napi_key = \"" + protected_key + "\"\n");

        const auto cfg = Config::load_from(path, ConfigSecretOptions{
                                                     .password_override = "legacy-password",
                                                 });
        expect(cfg.api_key == "sk-protected-legacy");
        expect((cfg.agents.contains("default")) >> fatal);
        expect(cfg.agents.at("default").api_key == "sk-protected-legacy");
    };

    "loads_protected_configured_agent_api_key_from_environment_password"_test = [] {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-protected-agent", "env-password", "agents.api_key");
        ScopedEnvVar config_password("ORANGUTAN_CONFIG_PASSWORD", "env-password");
        const auto path = harness.write_config("[agents.default]\napi_key = \"" + protected_key + "\"\n");

        const auto cfg = Config::load_from(path);
        expect((cfg.agents.contains("default")) >> fatal);
        expect(cfg.agents.at("default").api_key == "sk-protected-agent");
    };

    "named_agents_can_inherit_protected_legacy_api_key"_test = [] {
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
        expect((cfg.agents.contains("coder")) >> fatal);
        expect(cfg.agents.at("coder").api_key == "sk-inherited-agent");
    };

    "loads_protected_qq_client_secret_with_explicit_password"_test = [] {
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
        expect(cfg.qq_bots.size() == 1_ul);
        expect(cfg.qq_bots[0].client_secret == "qq-secret-value");
    };

    "throws_when_protected_secret_has_no_password_source"_test = [] {
        ConfigFileHarness harness;
        const auto protected_key = protect_config_secret("sk-no-password", "expected-password", "agent.api_key");
        const auto path = harness.write_config("[agent]\napi_key = \"" + protected_key + "\"\n");

        expect(throws<ConfigSecretProtectionError>([&] {
            static_cast<void>(Config::load_from(path));
        }));
    };

    "parses_edit_mode_from_tools_section"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[tools]
edit_mode = "search_replace"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.edit_mode == "search_replace");
    };

    "edit_mode_defaults_to_hashline_when_not_specified"_test = [] {
        ConfigFileHarness harness;
        const auto path = harness.write_config(R"toml(
[agent]
model = "test-model"
)toml");

        const auto cfg = Config::load_from(path);
        expect(cfg.edit_mode == "hashline");
    };

    "agents_inherit_global_edit_mode_and_can_override_it"_test = [] {
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
        expect((cfg.agents.contains("default")) >> fatal);
        expect((cfg.agents.contains("coder")) >> fatal);
        expect(cfg.agents.at("default").edit_mode == "search_replace");
        expect(cfg.agents.at("coder").edit_mode == "hashline");
    };
};

boost::ut::suite config_tool_filter_suite = [] {
    using namespace boost::ut;

    "empty_lists_allow_all"_test = [] {
        const Config cfg;
        expect(cfg.is_tool_allowed("shell"));
        expect(cfg.is_tool_allowed("read"));
        expect(cfg.is_tool_allowed("anything"));
    };

    "denied_tool_is_blocked"_test = [] {
        Config cfg;
        cfg.denied_tools = {"shell"};
        expect(not cfg.is_tool_allowed("shell"));
        expect(cfg.is_tool_allowed("read"));
    };

    "allowed_list_restricts_tools"_test = [] {
        Config cfg;
        cfg.allowed_tools = {"read", "ls"};
        expect(cfg.is_tool_allowed("read"));
        expect(cfg.is_tool_allowed("ls"));
        expect(not cfg.is_tool_allowed("shell"));
    };

    "denied_takes_precedence_over_allowed"_test = [] {
        Config cfg;
        cfg.allowed_tools = {"shell", "read"};
        cfg.denied_tools = {"shell"};
        expect(not cfg.is_tool_allowed("shell"));
        expect(cfg.is_tool_allowed("read"));
    };
};

boost::ut::suite config_env_expansion_suite = [] {
    using namespace boost::ut;

    "expand_env_vars_expands_defined_variable"_test = [] {
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);

        const auto result = expand_env_vars("${HOME}/config");
        expect(result == std::string(home) + "/config");
    };

    "expand_env_vars_undefined_variable_becomes_empty"_test = [] {
        const auto result = expand_env_vars("prefix_${ORANGUTAN_UNDEFINED_XYZ_12345}_suffix");
        expect(result == "prefix__suffix");
    };

    "expand_env_vars_multiple_variables"_test = [] {
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);

        const auto result = expand_env_vars("${HOME}:${HOME}");
        expect(result == std::string(home) + ":" + std::string(home));
    };

    "expand_env_vars_no_variables_passes_through"_test = [] {
        const auto result = expand_env_vars("plain text");
        expect(result == "plain text");
    };

    "expand_env_vars_empty_input_returns_empty"_test = [] {
        const auto result = expand_env_vars("");
        expect(result.empty());
    };

    "expand_env_vars_unclosed_brace_kept_literal"_test = [] {
        const auto result = expand_env_vars("prefix${NOCLOSE");
        expect(result == "prefix${NOCLOSE");
    };

    "expand_env_vars_adjacent_variables"_test = [] {
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);

        const auto result = expand_env_vars("${HOME}${HOME}");
        expect(result == std::string(home) + std::string(home));
    };

    "expand_home_path_expands_bare_home"_test = [] {
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);

        const auto result = expand_home_path("~");
        expect(result == std::string(home));
    };

    "expand_home_path_expands_home_prefix"_test = [] {
        const char *home = std::getenv("HOME");
        expect((home != nullptr) >> fatal);

        const auto result = expand_home_path("~/projects/orangutan");
        expect(result == std::string(home) + "/projects/orangutan");
    };

    "expand_home_path_leaves_plain_path_untouched"_test = [] {
        const auto result = expand_home_path("/tmp/orangutan");
        expect(result == "/tmp/orangutan");
    };
};

} // namespace
