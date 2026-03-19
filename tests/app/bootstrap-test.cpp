#include "app/bootstrap.hpp"
#include "app/channel-serve.hpp"

#include "app/runtime/identity.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "infra/config/config.hpp"
#include "infra/config/secret-protection.hpp"
#include "infra/storage/session-store.hpp"
#include "test-helpers.hpp"

#include <array>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

namespace orangutan::app::detail {

std::optional<std::unordered_map<std::string, AgentRuntimeConfig>> build_agent_runtime_configs(const orangutan::Config &cfg, const std::string &cli_api_key_override);

std::unordered_map<std::string, SubagentChildRuntimeConfig> build_subagent_child_runtime_configs(const std::unordered_map<std::string, AgentRuntimeConfig> &agent_runtime_configs);

} // namespace orangutan::app::detail

namespace {

class ScopedFdRedirect {
public:
    ScopedFdRedirect(int target_fd, int replacement_fd)
    : target_fd_(target_fd),
      saved_fd_(::dup(target_fd)) {
        if (saved_fd_ == -1) {
            throw std::runtime_error("dup failed");
        }
        if (::dup2(replacement_fd, target_fd_) == -1) {
            const auto saved = saved_fd_;
            saved_fd_ = -1;
            ::close(saved);
            throw std::runtime_error("dup2 failed");
        }
    }

    ~ScopedFdRedirect() {
        if (saved_fd_ == -1) {
            return;
        }
        ::dup2(saved_fd_, target_fd_);
        ::close(saved_fd_);
    }

    ScopedFdRedirect(const ScopedFdRedirect &) = delete;
    ScopedFdRedirect &operator=(const ScopedFdRedirect &) = delete;
    ScopedFdRedirect(ScopedFdRedirect &&) = delete;
    ScopedFdRedirect &operator=(ScopedFdRedirect &&) = delete;

private:
    int target_fd_;
    int saved_fd_;
};

class BootstrapTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = std::filesystem::current_path() / "tmp" / "tests" / "bootstrap";
        home_root_ = temp_root_ / "home";
        workspace_root_ = temp_root_ / "workspace";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(home_root_ / ".orangutan");
        std::filesystem::create_directories(workspace_root_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_root_);
    }

    [[nodiscard]]
    std::filesystem::path config_path() const {
        return home_root_ / ".orangutan" / "config.toml";
    }

    [[nodiscard]]
    std::filesystem::path session_db_path() const {
        return home_root_ / ".orangutan" / "sessions.db";
    }

    void write_config() const {
        write_config_with_api_key("test-key");
    }

    void write_config_with_api_key(const std::string &api_key) const {
        std::ofstream out(config_path());
        out << "[agents.default]\n";
        out << "provider = \"openai\"\n";
        out << "model = \"gpt-test\"\n";
        out << "base_url = \"https://example.test\"\n";
        out << "api_key = \"" << api_key << "\"\n";
        out << "workspace = \"" << workspace_root_.string() << "\"\n";
        out << "system_prompt = \"You are a test agent.\"\n";
    }

    void create_sessions() const {
        SessionStore session_store(session_db_path().string());
        const auto cli_identity = derive_cli_identity(workspace_root_.string(), "default");
        const std::vector<Message> history_a{
            Message{.role = "user", .content = {TextBlock{.text = "first"}}},
            Message{.role = "assistant", .content = {TextBlock{.text = "reply"}}},
        };
        const std::vector<Message> history_b{
            Message{.role = "user", .content = {TextBlock{.text = "second"}}},
            Message{.role = "assistant", .content = {TextBlock{.text = "reply two"}}},
        };
        session_store.save(history_a, "gpt-test", cli_identity.memory_scope);
        session_store.save(history_b, "gpt-test", cli_identity.memory_scope);
    }

    [[nodiscard]]
    const std::filesystem::path &home_root() const {
        return home_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

    struct RunResult {
        int exit_code = 0;
        std::string output;
    };

    [[nodiscard]]
    RunResult invoke_bootstrap(std::vector<std::string> argv_storage, const std::string &stdin_content = {}) const {
        std::array<int, 2> stdin_pipe{};
        EXPECT_EQ(::pipe(stdin_pipe.data()), 0);
        if (!stdin_content.empty()) {
            EXPECT_EQ(::write(stdin_pipe[1], stdin_content.data(), stdin_content.size()), static_cast<ssize_t>(stdin_content.size()));
        }
        ::close(stdin_pipe[1]);

        std::array<int, 2> output_pipe{};
        EXPECT_EQ(::pipe(output_pipe.data()), 0);

        int exit_code = 0;
        {
            ScopedFdRedirect redirect_stdin(STDIN_FILENO, stdin_pipe[0]);
            ScopedFdRedirect redirect_stdout(STDOUT_FILENO, output_pipe[1]);
            ScopedFdRedirect redirect_stderr(STDERR_FILENO, output_pipe[1]);
            ::close(stdin_pipe[0]);
            ::close(output_pipe[1]);

            std::vector<char *> argv;
            argv.reserve(argv_storage.size() + 1);
            for (auto &arg : argv_storage) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);

            exit_code = app::run_bootstrap(static_cast<int>(argv.size() - 1), argv.data());
        }

        std::string output;
        std::array<char, 256> buffer{};
        ssize_t read_bytes = 0;
        while ((read_bytes = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0) {
            output.append(buffer.data(), static_cast<size_t>(read_bytes));
        }
        ::close(output_pipe[0]);

        return {
            .exit_code = exit_code,
            .output = std::move(output),
        };
    }

private:
    std::filesystem::path temp_root_;
    std::filesystem::path home_root_;
    std::filesystem::path workspace_root_;
};

TEST_F(BootstrapTest, ResumeWithoutExplicitIdDoesNotConsumePipedMessageInput) {
    write_config();
    create_sessions();
    ScopedEnvVar home_env("HOME", home_root().string());

    std::array<int, 2> stdin_pipe{};
    ASSERT_EQ(::pipe(stdin_pipe.data()), 0);
    const std::string piped_input = "hello from stdin\n";
    ASSERT_EQ(::write(stdin_pipe[1], piped_input.data(), piped_input.size()), static_cast<ssize_t>(piped_input.size()));
    ::close(stdin_pipe[1]);

    std::array<int, 2> output_pipe{};
    ASSERT_EQ(::pipe(output_pipe.data()), 0);

    {
        ScopedFdRedirect redirect_stdin(STDIN_FILENO, stdin_pipe[0]);
        ScopedFdRedirect redirect_stdout(STDOUT_FILENO, output_pipe[1]);
        ScopedFdRedirect redirect_stderr(STDERR_FILENO, output_pipe[1]);
        ::close(stdin_pipe[0]);
        ::close(output_pipe[1]);

        std::vector<std::string> argv_storage{
            "orangutan", "--agent", "default", "--resume", "--event-stream", "--dump-session",
        };
        std::vector<char *> argv;
        argv.reserve(argv_storage.size() + 1);
        for (auto &arg : argv_storage) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);

        EXPECT_EQ(app::run_bootstrap(static_cast<int>(argv.size() - 1), argv.data()), 0);
    }

    ::close(output_pipe[0]);
}

TEST_F(BootstrapTest, BuildAgentRuntimeConfigsUsesPerAgentEditMode) {
    Config cfg;
    cfg.edit_mode = "hashline";
    cfg.agents.emplace("default", AgentConfig{
                                      .provider = "openai",
                                      .model = "gpt-test",
                                      .base_url = "https://example.test",
                                      .api_key = "test-key",
                                      .system_prompt = "You are a test agent.",
                                      .workspace = workspace_root().string(),
                                      .edit_mode = "hashline",
                                  });
    cfg.agents.emplace("coder", AgentConfig{
                                    .provider = "openai",
                                    .model = "gpt-coder",
                                    .base_url = "https://example.test",
                                    .api_key = "coder-key",
                                    .system_prompt = "You are a coder agent.",
                                    .workspace = workspace_root().string(),
                                    .edit_mode = "search_replace",
                                });

    const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
    ASSERT_TRUE(runtime_configs.has_value());
    auto default_it = runtime_configs->find("default");
    ASSERT_NE(default_it, runtime_configs->end());
    EXPECT_EQ(default_it->second.edit_mode, "hashline");
    auto coder_it = runtime_configs->find("coder");
    ASSERT_NE(coder_it, runtime_configs->end());
    EXPECT_EQ(coder_it->second.edit_mode, "search_replace");
}

TEST_F(BootstrapTest, BuildSubagentChildRuntimeConfigsPropagatesEditMode) {
    std::unordered_map<std::string, app::AgentRuntimeConfig> runtime_configs;
    runtime_configs.emplace("default", app::AgentRuntimeConfig{
                                           .agent_key = "default",
                                           .provider_name = "openai",
                                           .api_key = "test-key",
                                           .model = "gpt-test",
                                           .base_url = "https://example.test",
                                           .system_prompt = "You are a test agent.",
                                           .workspace_root = workspace_root().string(),
                                           .edit_mode = "search_replace",
                                       });

    const auto child_configs = app::detail::build_subagent_child_runtime_configs(runtime_configs);
    auto it = child_configs.find("default");
    ASSERT_NE(it, child_configs.end());
    EXPECT_EQ(it->second.edit_mode, "search_replace");
}

TEST_F(BootstrapTest, RunBootstrapLoadsProtectedConfigWithCliPassword) {
    const auto protected_key = protect_config_secret("test-key", "cli-password", "agents.api_key");
    write_config_with_api_key(protected_key);
    create_sessions();
    ScopedEnvVar home_env("HOME", home_root().string());

    const auto result = invoke_bootstrap({
        "orangutan",
        "--agent",
        "default",
        "--config-password",
        "cli-password",
        "--resume",
        "--event-stream",
        "--dump-session",
    });

    EXPECT_EQ(result.exit_code, 0);
}

TEST_F(BootstrapTest, RunBootstrapLoadsProtectedConfigWithEnvironmentPasswordHeadless) {
    const auto protected_key = protect_config_secret("test-key", "env-password", "agents.api_key");
    write_config_with_api_key(protected_key);
    create_sessions();
    ScopedEnvVar home_env("HOME", home_root().string());
    ScopedEnvVar password_env("ORANGUTAN_CONFIG_PASSWORD", "env-password");

    const auto result = invoke_bootstrap({
        "orangutan",
        "--agent",
        "default",
        "--resume",
        "--event-stream",
        "--dump-session",
    });

    EXPECT_EQ(result.exit_code, 0);
}

TEST_F(BootstrapTest, RunBootstrapFailsWithoutPasswordForProtectedConfigHeadless) {
    const auto protected_key = protect_config_secret("test-key", "missing-password", "agents.api_key");
    write_config_with_api_key(protected_key);
    create_sessions();
    ScopedEnvVar home_env("HOME", home_root().string());

    const auto result = invoke_bootstrap({
        "orangutan",
        "--agent",
        "default",
        "--resume",
        "--event-stream",
        "--dump-session",
    });

    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.output.find("--config-password"), std::string::npos);
    EXPECT_NE(result.output.find("ORANGUTAN_CONFIG_PASSWORD"), std::string::npos);
}

} // namespace
