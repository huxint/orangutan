#include "infra/config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace orangutan;

// ── Default config ──────────────────────────────

TEST(ConfigTest, DefaultValues) {
    const Config cfg;
    EXPECT_EQ(cfg.model, "claude-sonnet-4-20250514");
    EXPECT_EQ(cfg.base_url, "https://api.anthropic.com");
    EXPECT_DOUBLE_EQ(cfg.temperature, 1.0);
    EXPECT_EQ(cfg.max_iterations, 20);
    EXPECT_EQ(cfg.max_tokens, 4096);
    EXPECT_TRUE(cfg.workspace.empty());
    EXPECT_TRUE(cfg.allowed_tools.empty());
    EXPECT_TRUE(cfg.denied_tools.empty());
    EXPECT_EQ(cfg.permissions.sandbox_mode, ToolSandboxMode::isolated);
    EXPECT_EQ(cfg.permissions.shell_approval, ToolApprovalPolicy::ask);
    EXPECT_TRUE(cfg.permissions.denied_shell_commands.empty());
    EXPECT_TRUE(cfg.auto_save);
    EXPECT_FALSE(cfg.memory.mirror_enabled);
    EXPECT_EQ(cfg.memory.mirror_file, "MEMORY.md");
    EXPECT_EQ(cfg.memory.journal_dir, "memory");
}

// ── Load from TOML ─────────────────────────────

class ConfigFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "orangutan_config_test";
        std::filesystem::create_directories(tmp_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir_);
    }

    std::string write_config(const std::string &content) {
        auto path = (tmp_dir_ / "config.toml").string();
        std::ofstream ofs(path);
        ofs << content;
        return path;
    }

private:
    std::filesystem::path tmp_dir_;
};

TEST_F(ConfigFileTest, ParsesAgentSection) {
    auto path = write_config(R"toml(
[agent]
model = "claude-opus-4-20250514"
base_url = "http://localhost:8080"
temperature = 0.5
max_iterations = 10
max_tokens = 8192
workspace = "/tmp/orangutan-workspace"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.model, "claude-opus-4-20250514");
    EXPECT_EQ(cfg.base_url, "http://localhost:8080");
    EXPECT_DOUBLE_EQ(cfg.temperature, 0.5);
    EXPECT_EQ(cfg.max_iterations, 10);
    EXPECT_EQ(cfg.max_tokens, 8192);
    EXPECT_EQ(cfg.workspace, "/tmp/orangutan-workspace");
}

TEST_F(ConfigFileTest, ParsesToolsSection) {
    auto path = write_config(R"toml(
[tools]
allowed = ["read", "write", "ls"]
denied = ["shell"]
)toml");

    const auto cfg = Config::load_from(path);
    ASSERT_EQ(cfg.allowed_tools.size(), 3);
    EXPECT_EQ(cfg.allowed_tools[0], "read");
    EXPECT_EQ(cfg.allowed_tools[1], "write");
    EXPECT_EQ(cfg.allowed_tools[2], "ls");
    ASSERT_EQ(cfg.denied_tools.size(), 1);
    EXPECT_EQ(cfg.denied_tools[0], "shell");
    ASSERT_EQ(cfg.permissions.allowed_tools.size(), 3);
    EXPECT_EQ(cfg.permissions.allowed_tools[0], "read");
    EXPECT_EQ(cfg.permissions.allowed_tools[1], "write");
    EXPECT_EQ(cfg.permissions.allowed_tools[2], "ls");
    ASSERT_EQ(cfg.permissions.denied_tools.size(), 1);
    EXPECT_EQ(cfg.permissions.denied_tools[0], "shell");
}

TEST_F(ConfigFileTest, ParsesPermissionsSection) {
    auto path = write_config(R"toml(
[permissions]
sandbox_mode = "workspace-write"
shell_approval_policy = "deny"
allowed_tools = ["read", "write"]
denied_tools = ["shell"]
denied_shell_commands = ["rm -rf", "shutdown"]
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.permissions.sandbox_mode, ToolSandboxMode::workspace_write);
    EXPECT_EQ(cfg.permissions.shell_approval, ToolApprovalPolicy::deny);
    ASSERT_EQ(cfg.permissions.allowed_tools.size(), 2);
    EXPECT_EQ(cfg.permissions.allowed_tools[0], "read");
    EXPECT_EQ(cfg.permissions.allowed_tools[1], "write");
    ASSERT_EQ(cfg.permissions.denied_tools.size(), 1);
    EXPECT_EQ(cfg.permissions.denied_tools[0], "shell");
    ASSERT_EQ(cfg.permissions.denied_shell_commands.size(), 2);
    EXPECT_EQ(cfg.permissions.denied_shell_commands[0], "rm -rf");
    EXPECT_EQ(cfg.permissions.denied_shell_commands[1], "shutdown");
}

TEST_F(ConfigFileTest, ParsesCustomToolsSection) {
    auto path = write_config(R"toml(
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
    ASSERT_NE(home, nullptr);

    const auto cfg = Config::load_from(path);
    ASSERT_EQ(cfg.custom_tools.size(), 2);
    EXPECT_EQ(cfg.custom_tools[0].name, "ls");
    EXPECT_EQ(cfg.custom_tools[0].command, "ls -la ${path}");
    EXPECT_EQ(cfg.custom_tools[0].timeout, 15);
    EXPECT_EQ(cfg.custom_tools[0].working_dir, std::string(home) + "/workspace");
    EXPECT_EQ(cfg.custom_tools[0].input_schema.at("path"), "string");
    EXPECT_EQ(cfg.custom_tools[1].name, "grep");
    EXPECT_EQ(cfg.custom_tools[1].command, "rg --color=never -n ${pattern} ${path}");
    EXPECT_EQ(cfg.custom_tools[1].input_schema.at("pattern"), "string");
    EXPECT_EQ(cfg.custom_tools[1].input_schema.at("path"), "string");
}

TEST_F(ConfigFileTest, PartialConfigKeepsDefaults) {
    auto path = write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.model, "gpt-4");
    EXPECT_EQ(cfg.base_url, "https://api.anthropic.com");
    EXPECT_DOUBLE_EQ(cfg.temperature, 1.0);
    EXPECT_EQ(cfg.max_iterations, 20);
}

TEST_F(ConfigFileTest, EmptyFileReturnsDefaults) {
    auto path = write_config("");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.model, "claude-sonnet-4-20250514");
}

TEST_F(ConfigFileTest, InvalidTomlReturnsDefaults) {
    auto path = write_config("this is [not valid toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.model, "claude-sonnet-4-20250514");
}

TEST_F(ConfigFileTest, MissingFileReturnsDefaults) {
    const auto cfg = Config::load_from("/tmp/orangutan_nonexistent_config_xyz.toml");
    EXPECT_EQ(cfg.model, "claude-sonnet-4-20250514");
}

TEST_F(ConfigFileTest, ParsesSessionSection) {
    auto path = write_config(R"toml(
[session]
auto_save = true
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_TRUE(cfg.auto_save);
}

TEST_F(ConfigFileTest, ParsesMemorySection) {
    auto path = write_config(R"toml(
[memory]
mirror_enabled = true
mirror_file = "notes/MEMORY.md"
journal_dir = "journals"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_TRUE(cfg.memory.mirror_enabled);
    EXPECT_EQ(cfg.memory.mirror_file, "notes/MEMORY.md");
    EXPECT_EQ(cfg.memory.journal_dir, "journals");
}


TEST_F(ConfigFileTest, ParsesQqSection) {
    auto path = write_config(R"toml(
[qq]
app_id = "app-123"
client_secret = "secret-456"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.qq_app_id, "app-123");
    EXPECT_EQ(cfg.qq_client_secret, "secret-456");
    ASSERT_EQ(cfg.qq_bots.size(), 1);
    EXPECT_TRUE(cfg.qq_bots[0].name.empty());
    EXPECT_EQ(cfg.qq_bots[0].app_id, "app-123");
    EXPECT_EQ(cfg.qq_bots[0].client_secret, "secret-456");
    EXPECT_EQ(cfg.qq_bots[0].agent, "default");
}

TEST_F(ConfigFileTest, ParsesNamedAgentsAndQqBots) {
    auto path = write_config(R"toml(
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
    ASSERT_EQ(cfg.agents.size(), 2);
    ASSERT_TRUE(cfg.agents.contains("default"));
    ASSERT_TRUE(cfg.agents.contains("coder"));
    EXPECT_EQ(cfg.agents.at("default").model, "default-model");
    EXPECT_EQ(cfg.agents.at("default").subagents[0], "coder");
    EXPECT_EQ(cfg.agents.at("coder").provider, "openai");
    EXPECT_EQ(cfg.agents.at("coder").api_key, "coder-key");
    ASSERT_EQ(cfg.qq_bots.size(), 2);
    EXPECT_EQ(cfg.qq_bots[0].name, "primary");
    EXPECT_EQ(cfg.qq_bots[0].agent, "default");
    EXPECT_EQ(cfg.qq_bots[1].name, "coder-bot");
    EXPECT_EQ(cfg.qq_bots[1].agent, "coder");
}

TEST_F(ConfigFileTest, AgentPermissionsInheritGlobalDefaultsAndAllowOverrides) {
    auto path = write_config(R"toml(
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
    ASSERT_TRUE(cfg.agents.contains("default"));
    ASSERT_TRUE(cfg.agents.contains("coder"));

    const auto &default_agent = cfg.agents.at("default");
    EXPECT_EQ(default_agent.permissions.sandbox_mode, ToolSandboxMode::isolated);
    EXPECT_EQ(default_agent.permissions.shell_approval, ToolApprovalPolicy::allow);
    ASSERT_EQ(default_agent.permissions.allowed_tools.size(), 1);
    EXPECT_EQ(default_agent.permissions.allowed_tools[0], "read");
    ASSERT_EQ(default_agent.permissions.denied_shell_commands.size(), 1);
    EXPECT_EQ(default_agent.permissions.denied_shell_commands[0], "rm -rf");

    const auto &coder_agent = cfg.agents.at("coder");
    EXPECT_EQ(coder_agent.permissions.sandbox_mode, ToolSandboxMode::workspace_write);
    EXPECT_EQ(coder_agent.permissions.shell_approval, ToolApprovalPolicy::ask);
    ASSERT_EQ(coder_agent.permissions.allowed_tools.size(), 2);
    EXPECT_EQ(coder_agent.permissions.allowed_tools[0], "read");
    EXPECT_EQ(coder_agent.permissions.allowed_tools[1], "write");
    ASSERT_EQ(coder_agent.permissions.denied_tools.size(), 1);
    EXPECT_EQ(coder_agent.permissions.denied_tools[0], "shell");
    ASSERT_EQ(coder_agent.permissions.denied_shell_commands.size(), 1);
    EXPECT_EQ(coder_agent.permissions.denied_shell_commands[0], "curl");
}

TEST_F(ConfigFileTest, ParsesSecuritySection) {
    auto path = write_config(R"toml(
[security]
allow = ["cli:*", "qqbot:c2c:*"]
deny = ["qqbot:c2c:blocked"]
)toml");

    const auto cfg = Config::load_from(path);
    ASSERT_EQ(cfg.allow.size(), 2);
    EXPECT_EQ(cfg.allow[0], "cli:*");
    EXPECT_EQ(cfg.allow[1], "qqbot:c2c:*");
    ASSERT_EQ(cfg.deny.size(), 1);
    EXPECT_EQ(cfg.deny[0], "qqbot:c2c:blocked");
}

TEST_F(ConfigFileTest, ParsesMcpServersSection) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto path = write_config(R"toml(
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
    ASSERT_EQ(cfg.mcp_servers.size(), 1);
    const auto &server = cfg.mcp_servers[0];
    EXPECT_EQ(server.name, "github");
    EXPECT_EQ(server.command, std::string(home) + "/bin/mock-mcp");
    ASSERT_EQ(server.args.size(), 2);
    EXPECT_EQ(server.args[0], "--token");
    EXPECT_EQ(server.args[1], std::string(home) + "/token.txt");
    EXPECT_EQ(server.timeout, 12);
    EXPECT_EQ(server.env.at("GITHUB_TOKEN"), "gh-test");
    EXPECT_EQ(server.env.at("ROOT"), std::string(home) + "/workspace");
}

TEST_F(ConfigFileTest, SkipsInvalidMcpServers) {
    auto path = write_config(R"toml(
[[mcp.servers]]
command = "missing-name"

[[mcp.servers]]
name = "missing-command"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_TRUE(cfg.mcp_servers.empty());
}

TEST_F(ConfigFileTest, LaterMcpServerOverridesEarlierDuplicateName) {
    auto path = write_config(R"toml(
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
    ASSERT_EQ(cfg.mcp_servers.size(), 1);
    EXPECT_EQ(cfg.mcp_servers[0].name, "github");
    EXPECT_EQ(cfg.mcp_servers[0].command, "new-command");
    EXPECT_EQ(cfg.mcp_servers[0].timeout, 9);
}

// ── Tool allow/deny ─────────────────────────────

TEST(ConfigToolFilterTest, EmptyListsAllowAll) {
    const Config cfg;
    EXPECT_TRUE(cfg.is_tool_allowed("shell"));
    EXPECT_TRUE(cfg.is_tool_allowed("read"));
    EXPECT_TRUE(cfg.is_tool_allowed("anything"));
}

TEST(ConfigToolFilterTest, DeniedToolIsBlocked) {
    Config cfg;
    cfg.denied_tools = {"shell"};
    EXPECT_FALSE(cfg.is_tool_allowed("shell"));
    EXPECT_TRUE(cfg.is_tool_allowed("read"));
}

TEST(ConfigToolFilterTest, AllowedListRestrictsTools) {
    Config cfg;
    cfg.allowed_tools = {"read", "ls"};
    EXPECT_TRUE(cfg.is_tool_allowed("read"));
    EXPECT_TRUE(cfg.is_tool_allowed("ls"));
    EXPECT_FALSE(cfg.is_tool_allowed("shell"));
}

TEST(ConfigToolFilterTest, DeniedTakesPrecedenceOverAllowed) {
    Config cfg;
    cfg.allowed_tools = {"shell", "read"};
    cfg.denied_tools = {"shell"};
    EXPECT_FALSE(cfg.is_tool_allowed("shell"));
    EXPECT_TRUE(cfg.is_tool_allowed("read"));
}

// ── Environment variable expansion ──────────────

TEST(ExpandEnvVarsTest, ExpandsDefinedVariable) {
    // HOME should always be set in test environments
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto result = expand_env_vars("${HOME}/config");
    EXPECT_EQ(result, std::string(home) + "/config");
}

TEST(ExpandEnvVarsTest, UndefinedVariableBecomesEmpty) {
    auto result = expand_env_vars("prefix_${ORANGUTAN_UNDEFINED_XYZ_12345}_suffix");
    EXPECT_EQ(result, "prefix__suffix");
}

TEST(ExpandEnvVarsTest, MultipleVariables) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto result = expand_env_vars("${HOME}:${HOME}");
    EXPECT_EQ(result, std::string(home) + ":" + std::string(home));
}

TEST(ExpandEnvVarsTest, NoVariablesPassesThrough) {
    auto result = expand_env_vars("plain text");
    EXPECT_EQ(result, "plain text");
}

TEST(ExpandEnvVarsTest, EmptyInputReturnsEmpty) {
    auto result = expand_env_vars("");
    EXPECT_EQ(result, "");
}

TEST(ExpandEnvVarsTest, UnclosedBraceKeptLiteral) {
    auto result = expand_env_vars("prefix${NOCLOSE");
    EXPECT_EQ(result, "prefix${NOCLOSE");
}

TEST(ExpandEnvVarsTest, AdjacentVariables) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto result = expand_env_vars("${HOME}${HOME}");
    EXPECT_EQ(result, std::string(home) + std::string(home));
}

TEST(ExpandHomePathTest, ExpandsBareHome) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto result = expand_home_path("~");
    EXPECT_EQ(result, std::string(home));
}

TEST(ExpandHomePathTest, ExpandsHomePrefix) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto result = expand_home_path("~/projects/orangutan");
    EXPECT_EQ(result, std::string(home) + "/projects/orangutan");
}

TEST(ExpandHomePathTest, LeavesPlainPathUntouched) {
    auto result = expand_home_path("/tmp/orangutan");
    EXPECT_EQ(result, "/tmp/orangutan");
}

// ── api_key from config file ────────────────────

TEST_F(ConfigFileTest, ParsesApiKey) {
    auto path = write_config(R"toml(
[agent]
api_key = "sk-test-key-12345"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.api_key, "sk-test-key-12345");
}

TEST_F(ConfigFileTest, ApiKeyWithEnvVarExpansion) {
    // Set a temporary env var for testing
    setenv("ORANGUTAN_TEST_KEY", "sk-from-env", 1);

    auto path = write_config(R"toml(
[agent]
api_key = "${ORANGUTAN_TEST_KEY}"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.api_key, "sk-from-env");

    unsetenv("ORANGUTAN_TEST_KEY");
}

TEST_F(ConfigFileTest, EnvVarExpansionInBaseUrl) {
    setenv("ORANGUTAN_TEST_HOST", "myhost", 1);

    auto path = write_config(R"toml(
[agent]
base_url = "http://${ORANGUTAN_TEST_HOST}:8080"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.base_url, "http://myhost:8080");

    unsetenv("ORANGUTAN_TEST_HOST");
}

TEST_F(ConfigFileTest, EnvVarExpansionInWorkspace) {
    setenv("ORANGUTAN_TEST_WORKSPACE", "/tmp/orangutan-workspace-from-env", 1);

    auto path = write_config(R"toml(
[agent]
workspace = "${ORANGUTAN_TEST_WORKSPACE}"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.workspace, "/tmp/orangutan-workspace-from-env");

    unsetenv("ORANGUTAN_TEST_WORKSPACE");
}

TEST_F(ConfigFileTest, TildeExpansionInWorkspace) {
    const char *home = std::getenv("HOME");
    ASSERT_NE(home, nullptr);

    auto path = write_config(R"toml(
[agent]
workspace = "~/projects/orangutan"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_EQ(cfg.workspace, std::string(home) + "/projects/orangutan");
}

TEST_F(ConfigFileTest, DefaultApiKeyIsEmpty) {
    auto path = write_config(R"toml(
[agent]
model = "gpt-4"
)toml");

    const auto cfg = Config::load_from(path);
    EXPECT_TRUE(cfg.api_key.empty());
}
