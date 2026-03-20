#include "app/bootstrap.hpp"
#include "app/channel-serve.hpp"
#include "app/runtime/agent-runtime.hpp"

#include "app/runtime/identity.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/subagent/subagent-manager.hpp"
#include "features/web/web-server.hpp"
#include "infra/config/config.hpp"
#include "infra/config/secret-protection.hpp"
#include "infra/storage/session-store.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

namespace orangutan::app::detail {

struct WebStartupInspection {
    bool has_session_store = false;
    bool has_memory_store = false;
    bool has_subagent_run_store = false;
    bool has_subagent_manager = false;
    bool has_runtime_bundle = false;
    bool has_runtime_agent = false;
    bool attached_session_store = false;
    bool attached_tool_registry = false;
    bool attached_skill_loader = false;
    std::vector<ToolDef> tool_definitions;
    std::vector<std::string> active_skill_names;
    std::string runtime_build_error;
};

void set_web_startup_inspection_callback_for_tests(std::function<bool(const WebStartupInspection &)> callback);
void clear_web_startup_inspection_callback_for_tests();
void set_web_runtime_build_callback_for_tests(std::function<void()> callback);
void clear_web_runtime_build_callback_for_tests();

} // namespace orangutan::app::detail

namespace {

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&name](const ToolDef &definition) {
        return definition.name == name;
    });
}

namespace bootstrap_detail = orangutan::app::detail;
class ScopedWebStartupInspectionCapture {
public:
    ScopedWebStartupInspectionCapture() {
        bootstrap_detail::set_web_startup_inspection_callback_for_tests([this](const bootstrap_detail::WebStartupInspection &inspection) {
            inspection_ = inspection;
            return true;
        });
    }

    ~ScopedWebStartupInspectionCapture() {
        bootstrap_detail::clear_web_startup_inspection_callback_for_tests();
    }

    ScopedWebStartupInspectionCapture(const ScopedWebStartupInspectionCapture &) = delete;
    ScopedWebStartupInspectionCapture &operator=(const ScopedWebStartupInspectionCapture &) = delete;
    ScopedWebStartupInspectionCapture(ScopedWebStartupInspectionCapture &&) = delete;
    ScopedWebStartupInspectionCapture &operator=(ScopedWebStartupInspectionCapture &&) = delete;

    [[nodiscard]]
    const std::optional<bootstrap_detail::WebStartupInspection> &inspection() const {
        return inspection_;
    }

private:
    std::optional<bootstrap_detail::WebStartupInspection> inspection_;
};
class ScopedUnsetEnvVar {
public:
    explicit ScopedUnsetEnvVar(const char *name)
    : name_(name) {
        if (const auto *current = std::getenv(name); current != nullptr) {
            previous_ = current;
        }
        unsetenv(name_.c_str());
    }

    ~ScopedUnsetEnvVar() {
        if (previous_.has_value()) {
            setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedUnsetEnvVar(const ScopedUnsetEnvVar &) = delete;
    ScopedUnsetEnvVar &operator=(const ScopedUnsetEnvVar &) = delete;
    ScopedUnsetEnvVar(ScopedUnsetEnvVar &&) = delete;
    ScopedUnsetEnvVar &operator=(ScopedUnsetEnvVar &&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};
class ScopedWebRuntimeBuildFailureInjection {
public:
    explicit ScopedWebRuntimeBuildFailureInjection(std::string message)
    : message_(std::move(message)) {
        bootstrap_detail::set_web_runtime_build_callback_for_tests([message = message_]() {
            throw std::runtime_error(message);
        });
    }

    ~ScopedWebRuntimeBuildFailureInjection() {
        bootstrap_detail::clear_web_runtime_build_callback_for_tests();
    }

    ScopedWebRuntimeBuildFailureInjection(const ScopedWebRuntimeBuildFailureInjection &) = delete;
    ScopedWebRuntimeBuildFailureInjection &operator=(const ScopedWebRuntimeBuildFailureInjection &) = delete;
    ScopedWebRuntimeBuildFailureInjection(ScopedWebRuntimeBuildFailureInjection &&) = delete;
    ScopedWebRuntimeBuildFailureInjection &operator=(ScopedWebRuntimeBuildFailureInjection &&) = delete;

private:
    std::string message_;
};

struct BootstrapRunResult {
    int exit_code = 0;
    std::string output;
};

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

class PtyBootstrapSession {
public:
    explicit PtyBootstrapSession(std::vector<std::string> argv_storage)
    : argv_storage_(std::move(argv_storage)) {
        master_fd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_fd_ == -1) {
            throw std::runtime_error("posix_openpt failed");
        }
        if (::grantpt(master_fd_) != 0) {
            throw std::runtime_error("grantpt failed");
        }
        if (::unlockpt(master_fd_) != 0) {
            throw std::runtime_error("unlockpt failed");
        }

        std::array<char, 256> slave_name{};
        if (::ptsname_r(master_fd_, slave_name.data(), slave_name.size()) != 0) {
            throw std::runtime_error("ptsname_r failed");
        }

        slave_fd_ = ::open(slave_name.data(), O_RDWR | O_NOCTTY);
        if (slave_fd_ == -1) {
            throw std::runtime_error("open pty slave failed");
        }

        const int flags = ::fcntl(master_fd_, F_GETFL, 0);
        if (flags == -1 || ::fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("fcntl failed");
        }

        future_ = std::async(std::launch::async, [this] {
            ScopedFdRedirect redirect_stdin(STDIN_FILENO, slave_fd_);
            ScopedFdRedirect redirect_stdout(STDOUT_FILENO, slave_fd_);
            ScopedFdRedirect redirect_stderr(STDERR_FILENO, slave_fd_);

            std::vector<char *> argv;
            argv.reserve(argv_storage_.size() + 1);
            for (auto &arg : argv_storage_) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);

            return app::run_bootstrap(static_cast<int>(argv.size() - 1), argv.data());
        });
    }

    ~PtyBootstrapSession() {
        close_fds();
    }

    PtyBootstrapSession(const PtyBootstrapSession &) = delete;
    PtyBootstrapSession &operator=(const PtyBootstrapSession &) = delete;
    PtyBootstrapSession(PtyBootstrapSession &&) = delete;
    PtyBootstrapSession &operator=(PtyBootstrapSession &&) = delete;

    [[nodiscard]]
    bool wait_for_output(std::string_view needle, std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            drain_output();
            if (output_.find(needle) != std::string::npos) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        drain_output();
        return output_.find(needle) != std::string::npos;
    }

    [[nodiscard]]
    std::optional<int> wait_for_web_port(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            drain_output();
            if (const auto port = parse_web_port(output_); port.has_value()) {
                return port;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        drain_output();
        return parse_web_port(output_);
    }

    void write_input(std::string_view input) const {
        if (master_fd_ == -1) {
            throw std::runtime_error("pty master is closed");
        }
        size_t written_total = 0;
        while (written_total < input.size()) {
            const auto written = ::write(master_fd_, input.data() + written_total, input.size() - written_total);
            if (written <= 0) {
                throw std::runtime_error("failed to write to pty master");
            }
            written_total += static_cast<size_t>(written);
        }
    }

    [[nodiscard]]
    BootstrapRunResult finish(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        if (finished_.has_value()) {
            return *finished_;
        }
        if (future_.wait_for(timeout) != std::future_status::ready) {
            throw std::runtime_error("bootstrap did not finish before timeout");
        }
        const auto exit_code = future_.get();
        drain_output();
        finished_ = BootstrapRunResult{
            .exit_code = exit_code,
            .output = output_,
        };
        close_fds();
        return *finished_;
    }

    [[nodiscard]]
    const std::string &output() {
        drain_output();
        return output_;
    }

private:
    static std::optional<int> parse_web_port(const std::string &output) {
        constexpr std::string_view marker = "Web UI available at http://127.0.0.1:";
        const auto pos = output.rfind(marker);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        auto cursor = pos + marker.size();
        auto end = cursor;
        while (end < output.size() && std::isdigit(static_cast<unsigned char>(output[end])) != 0) {
            ++end;
        }
        if (end == cursor) {
            return std::nullopt;
        }
        return std::stoi(output.substr(cursor, end - cursor));
    }

    void drain_output() {
        if (master_fd_ == -1) {
            return;
        }

        std::array<char, 512> buffer{};
        while (true) {
            const auto read_bytes = ::read(master_fd_, buffer.data(), buffer.size());
            if (read_bytes > 0) {
                output_.append(buffer.data(), static_cast<size_t>(read_bytes));
                continue;
            }
            if (read_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            if (read_bytes <= 0) {
                break;
            }
        }
    }

    void close_fds() {
        if (slave_fd_ != -1) {
            ::close(slave_fd_);
            slave_fd_ = -1;
        }
        if (master_fd_ != -1) {
            ::close(master_fd_);
            master_fd_ = -1;
        }
    }

    std::vector<std::string> argv_storage_;
    int master_fd_ = -1;
    int slave_fd_ = -1;
    std::future<int> future_;
    std::string output_;
    std::optional<BootstrapRunResult> finished_;
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

    static void write_skill(const std::filesystem::path &base_dir, const std::string &dir_name, const std::string &skill_name, const std::string &body) {
        const auto skill_dir = base_dir / dir_name;
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "+++\n";
        out << "name = \"" << skill_name << "\"\n";
        out << "description = \"bootstrap test skill\"\n";
        out << "+++\n\n";
        out << body << "\n";
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
        const SessionMetadata metadata{
            .model = "gpt-test",
            .scope_key = cli_identity.memory_scope,
            .agent_key = "default",
            .origin_kind = "cli",
            .origin_ref = "cli:local",
        };
        session_store.save(history_a, metadata);
        session_store.save(history_b, metadata);
    }

    [[nodiscard]]
    const std::filesystem::path &temp_root() const {
        return temp_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &home_root() const {
        return home_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

    using RunResult = BootstrapRunResult;

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

TEST_F(BootstrapTest, BuildEffectiveAgentsAddsLegacyDefaultWhenMissing) {
    Config cfg;
    cfg.provider = "openai";
    cfg.model = "gpt-test";
    cfg.base_url = "https://example.test";
    cfg.api_key = "test-key";
    cfg.workspace = workspace_root().string();

    const auto agents = app::detail::build_effective_agents(cfg);
    auto it = agents.find("default");
    ASSERT_NE(it, agents.end());
    EXPECT_EQ(it->second.model, "gpt-test");
    EXPECT_EQ(it->second.workspace, workspace_root().string());
}

TEST_F(BootstrapTest, BuildEffectiveAgentsAssignsDefaultWorkspaceWhenMissing) {
    ScopedEnvVar home_env("HOME", home_root().string());

    Config cfg;
    cfg.provider = "openai";
    cfg.model = "gpt-test";
    cfg.base_url = "https://example.test";
    cfg.api_key = "test-key";
    cfg.agents.emplace("coder", AgentConfig{
                                    .provider = "openai",
                                    .model = "gpt-coder",
                                    .base_url = "https://example.test",
                                    .api_key = "coder-key",
                                });

    const auto agents = app::detail::build_effective_agents(cfg);
    const auto expected = (home_root() / ".orangutan" / "workspace" / "main").lexically_normal().string();

    ASSERT_EQ(agents.count("default"), 1U);
    ASSERT_EQ(agents.count("coder"), 1U);
    EXPECT_EQ(agents.at("default").workspace, expected);
    EXPECT_EQ(agents.at("coder").workspace, expected);
}

TEST_F(BootstrapTest, BuildAgentRuntimeConfigsAddsLegacyDefaultWhenMissing) {
    Config cfg;
    cfg.provider = "openai";
    cfg.model = "gpt-test";
    cfg.base_url = "https://example.test";
    cfg.api_key = "test-key";
    cfg.workspace = workspace_root().string();

    const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
    ASSERT_TRUE(runtime_configs.has_value());
    auto it = runtime_configs->find("default");
    ASSERT_NE(it, runtime_configs->end());
    EXPECT_EQ(it->second.agent_key, "default");
    EXPECT_EQ(it->second.model, "gpt-test");
}

TEST_F(BootstrapTest, BuildAgentRuntimeConfigsAssignsDefaultWorkspaceWhenMissing) {
    ScopedEnvVar home_env("HOME", home_root().string());

    Config cfg;
    cfg.agents.emplace("default", AgentConfig{
                                      .provider = "openai",
                                      .model = "gpt-test",
                                      .base_url = "https://example.test",
                                      .api_key = "test-key",
                                  });

    const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
    ASSERT_TRUE(runtime_configs.has_value());
    auto it = runtime_configs->find("default");
    ASSERT_NE(it, runtime_configs->end());

    const auto expected = std::filesystem::weakly_canonical(home_root() / ".orangutan" / "workspace" / "main").string();
    EXPECT_EQ(it->second.workspace_root, expected);
    EXPECT_TRUE(std::filesystem::exists(expected));
    EXPECT_TRUE(std::filesystem::is_directory(expected));
}

TEST_F(BootstrapTest, BuildAgentRuntimeConfigsPreservesDefaultSubagents) {
    Config cfg;
    cfg.agents.emplace("default", AgentConfig{
                                      .provider = "openai",
                                      .model = "gpt-test",
                                      .base_url = "https://example.test",
                                      .api_key = "test-key",
                                      .workspace = workspace_root().string(),
                                      .subagents = {"coder"},
                                  });
    cfg.agents.emplace("coder", AgentConfig{
                                    .provider = "openai",
                                    .model = "gpt-coder",
                                    .base_url = "https://example.test",
                                    .api_key = "coder-key",
                                    .workspace = workspace_root().string(),
                                });

    const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
    ASSERT_TRUE(runtime_configs.has_value());
    auto default_it = runtime_configs->find("default");
    ASSERT_NE(default_it, runtime_configs->end());
    ASSERT_EQ(default_it->second.allowed_child_agents.size(), 1U);
    EXPECT_EQ(default_it->second.allowed_child_agents.front(), "coder");
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

TEST_F(BootstrapTest, ReplRuntimeListsMemoryToolsAndSkills) {
    write_config();
    write_skill(workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
    ScopedEnvVar home_env("HOME", home_root().string());
    ScopedEnvVar term_env("TERM", "dumb");

    PtyBootstrapSession session({"orangutan", "--agent", "default"});
    ASSERT_TRUE(session.wait_for_output("Type /help"));

    session.write_input("/tools\n");
    session.write_input("/skills\n");
    session.write_input("/quit\n");

    const auto result = session.finish();
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.output.find("memory_list"), std::string::npos);
    EXPECT_NE(result.output.find("workspace-skill"), std::string::npos);
}

TEST_F(BootstrapTest, WebModeBuildsAndAttachesRealBootstrapRuntimeDependencies) {
    write_config();
    write_skill(workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
    ScopedEnvVar home_env("HOME", home_root().string());
    ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
    ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
    ScopedWebStartupInspectionCapture inspection_capture;

    const auto result = invoke_bootstrap({"orangutan", "--agent", "default", "--web", "--port", "0"});

    ASSERT_EQ(result.exit_code, 0) << result.output;
    ASSERT_TRUE(inspection_capture.inspection().has_value());
    const auto &inspection = *inspection_capture.inspection();
    EXPECT_TRUE(inspection.has_session_store);
    EXPECT_TRUE(inspection.has_memory_store);
    EXPECT_TRUE(inspection.has_subagent_run_store);
    EXPECT_TRUE(inspection.has_subagent_manager);
    EXPECT_TRUE(inspection.has_runtime_bundle);
    EXPECT_TRUE(inspection.has_runtime_agent);
    EXPECT_TRUE(inspection.attached_session_store);
    EXPECT_TRUE(inspection.attached_tool_registry);
    EXPECT_TRUE(inspection.attached_skill_loader);
    EXPECT_TRUE(has_tool_named(inspection.tool_definitions, "memory_list"));
    EXPECT_NE(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")), inspection.active_skill_names.end());
}

TEST_F(BootstrapTest, WebOnlyCreatesWebAssemblyDependenciesWithoutApiKey) {
    write_config_with_api_key("");
    write_skill(workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
    ScopedEnvVar home_env("HOME", home_root().string());
    ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
    ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
    ScopedWebStartupInspectionCapture inspection_capture;

    const auto result = invoke_bootstrap({"orangutan", "--agent", "default", "--web-only", "--port", "0"});

    ASSERT_EQ(result.exit_code, 0) << result.output;
    ASSERT_TRUE(inspection_capture.inspection().has_value());
    const auto &inspection = *inspection_capture.inspection();
    EXPECT_TRUE(inspection.has_session_store);
    EXPECT_TRUE(inspection.has_memory_store);
    EXPECT_TRUE(inspection.has_subagent_run_store);
    EXPECT_TRUE(inspection.has_subagent_manager);
    EXPECT_FALSE(inspection.has_runtime_bundle);
    EXPECT_FALSE(inspection.has_runtime_agent);
    EXPECT_TRUE(inspection.attached_session_store);
    EXPECT_FALSE(inspection.attached_tool_registry);
    EXPECT_TRUE(inspection.attached_skill_loader);
    EXPECT_TRUE(inspection.tool_definitions.empty());
    EXPECT_NE(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")), inspection.active_skill_names.end());
}

TEST_F(BootstrapTest, WebOnlyStartsAdminUiWhenRuntimeAssemblyFails) {
    write_config();
    write_skill(workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
    ScopedEnvVar home_env("HOME", home_root().string());
    ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
    ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
    ScopedWebStartupInspectionCapture inspection_capture;
    ScopedWebRuntimeBuildFailureInjection build_failure("injected web-only runtime failure");

    const auto result = invoke_bootstrap({"orangutan", "--agent", "default", "--web-only", "--port", "0"});

    ASSERT_EQ(result.exit_code, 0) << result.output;
    ASSERT_TRUE(inspection_capture.inspection().has_value());
    const auto &inspection = *inspection_capture.inspection();
    EXPECT_TRUE(inspection.has_session_store);
    EXPECT_TRUE(inspection.has_memory_store);
    EXPECT_TRUE(inspection.has_subagent_run_store);
    EXPECT_TRUE(inspection.has_subagent_manager);
    EXPECT_FALSE(inspection.has_runtime_bundle);
    EXPECT_FALSE(inspection.has_runtime_agent);
    EXPECT_TRUE(inspection.attached_session_store);
    EXPECT_FALSE(inspection.attached_tool_registry);
    EXPECT_TRUE(inspection.attached_skill_loader);
    EXPECT_TRUE(inspection.tool_definitions.empty());
    EXPECT_NE(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")), inspection.active_skill_names.end());
    EXPECT_NE(inspection.runtime_build_error.find("injected web-only runtime failure"), std::string::npos);
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
