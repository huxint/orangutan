#include "app/bootstrap.hpp"
#include "app/channel-serve.hpp"
#include "app/runtime/agent-runtime.hpp"
#include "app/runtime/app-runtime.hpp"

#include "app/runtime/identity.hpp"
#include "features/skills/skill-loader.hpp"
#include "features/memory/memory.hpp"
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
#include "support/ut.hpp"
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
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
void set_channel_mode_callback_for_tests(std::function<int()> callback);
void clear_channel_mode_callback_for_tests();

} // namespace orangutan::app::detail

namespace {

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

class ScopedChannelModeCallback {
public:
    explicit ScopedChannelModeCallback(std::function<int()> callback) {
        bootstrap_detail::set_channel_mode_callback_for_tests(std::move(callback));
    }

    ~ScopedChannelModeCallback() {
        bootstrap_detail::clear_channel_mode_callback_for_tests();
    }

    ScopedChannelModeCallback(const ScopedChannelModeCallback &) = delete;
    ScopedChannelModeCallback &operator=(const ScopedChannelModeCallback &) = delete;
    ScopedChannelModeCallback(ScopedChannelModeCallback &&) = delete;
    ScopedChannelModeCallback &operator=(ScopedChannelModeCallback &&) = delete;
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

class ScopedPipe {
public:
    ScopedPipe() {
        if (::pipe(fds_.data()) != 0) {
            throw std::runtime_error("pipe failed");
        }
    }

    ~ScopedPipe() {
        close_read();
        close_write();
    }

    ScopedPipe(const ScopedPipe &) = delete;
    ScopedPipe &operator=(const ScopedPipe &) = delete;
    ScopedPipe(ScopedPipe &&) = delete;
    ScopedPipe &operator=(ScopedPipe &&) = delete;

    [[nodiscard]]
    int read_end() const {
        return fds_[0];
    }

    [[nodiscard]]
    int write_end() const {
        return fds_[1];
    }

    void close_read() {
        if (fds_[0] != -1) {
            ::close(fds_[0]);
            fds_[0] = -1;
        }
    }

    void close_write() {
        if (fds_[1] != -1) {
            ::close(fds_[1]);
            fds_[1] = -1;
        }
    }

    void write_all(std::string_view content) {
        if (fds_[1] == -1) {
            throw std::runtime_error("pipe write end is closed");
        }

        size_t written_total = 0;
        while (written_total < content.size()) {
            const auto remaining = content.substr(written_total);
            const auto written = ::write(fds_[1], remaining.data(), remaining.size());
            if (written <= 0) {
                throw std::runtime_error("pipe write failed");
            }
            written_total += static_cast<size_t>(written);
        }
    }

private:
    std::array<int, 2> fds_{-1, -1};
};

class BootstrapHarness {
public:
    BootstrapHarness()
    : temp_root_(orangutan::testing::unique_test_root("bootstrap")),
      home_root_(temp_root_ / "home"),
      workspace_root_(temp_root_ / "workspace") {
        std::filesystem::create_directories(home_root_ / ".orangutan");
        std::filesystem::create_directories(workspace_root_);
    }

    ~BootstrapHarness() {
        std::filesystem::remove_all(temp_root_);
    }
    BootstrapHarness(const BootstrapHarness &) = delete;
    BootstrapHarness &operator=(const BootstrapHarness &) = delete;
    BootstrapHarness(BootstrapHarness &&) = delete;
    BootstrapHarness &operator=(BootstrapHarness &&) = delete;

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
        SessionStore session_store(session_db_path());
        const auto cli_identity = derive_cli_identity(workspace_root_.string(), "default");
        const std::vector<Message> history_a{
            Message{.role = Role::User, .content = {TextBlock{.text = "first"}}},
            Message{.role = Role::Assistant, .content = {TextBlock{.text = "reply"}}},
        };
        const std::vector<Message> history_b{
            Message{.role = Role::User, .content = {TextBlock{.text = "second"}}},
            Message{.role = Role::Assistant, .content = {TextBlock{.text = "reply two"}}},
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
    static RunResult invoke_bootstrap(std::vector<std::string> argv_storage, const std::string &stdin_content = {}) {
        ScopedPipe stdin_pipe;
        if (!stdin_content.empty()) {
            stdin_pipe.write_all(stdin_content);
        }
        stdin_pipe.close_write();

        ScopedPipe output_pipe;

        int exit_code = 0;
        {
            ScopedFdRedirect redirect_stdin(STDIN_FILENO, stdin_pipe.read_end());
            ScopedFdRedirect redirect_stdout(STDOUT_FILENO, output_pipe.write_end());
            ScopedFdRedirect redirect_stderr(STDERR_FILENO, output_pipe.write_end());
            stdin_pipe.close_read();
            output_pipe.close_write();

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
        while ((read_bytes = ::read(output_pipe.read_end(), buffer.data(), buffer.size())) > 0) {
            output.append(buffer.data(), static_cast<size_t>(read_bytes));
        }
        output_pipe.close_read();

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

// Boost.UT registers suites through static initialization at namespace scope.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const boost::ut::suite bootstrap_suite = [] {
    using namespace boost::ut;

    "resume_without_explicit_id_does_not_consume_piped_message_input"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        harness.create_sessions();
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        ScopedPipe stdin_pipe;
        const std::string piped_input = "hello from stdin\n";
        stdin_pipe.write_all(piped_input);
        stdin_pipe.close_write();

        ScopedPipe output_pipe;

        {
            ScopedFdRedirect redirect_stdin(STDIN_FILENO, stdin_pipe.read_end());
            ScopedFdRedirect redirect_stdout(STDOUT_FILENO, output_pipe.write_end());
            ScopedFdRedirect redirect_stderr(STDERR_FILENO, output_pipe.write_end());
            stdin_pipe.close_read();
            output_pipe.close_write();

            std::vector<std::string> argv_storage{
                "orangutan", "--cli", "--agent", "default", "--resume", "--event-stream", "--dump-session",
            };
            std::vector<char *> argv;
            argv.reserve(argv_storage.size() + 1);
            for (auto &arg : argv_storage) {
                argv.push_back(arg.data());
            }
            argv.push_back(nullptr);

            expect(app::run_bootstrap(static_cast<int>(argv.size() - 1), argv.data()) == 0_i);
        }

        output_pipe.close_read();
    };

    "run_bootstrap_requires_at_least_one_entry_flag"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--agent", "default"});

        expect(result.exit_code == 1_i);
        expect(result.output.contains("specify at least one entry flag"));
    };

    "channel_only_mode_returns_startup_failure_instead_of_hanging"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedChannelModeCallback channel_failure([] {
            return 1;
        });

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--channel"});

        expect(result.exit_code == 1_i);
    };

    "build_agent_runtime_configs_uses_per_agent_edit_mode"_test = [] {
        BootstrapHarness harness;
        Config cfg;
        cfg.edit_mode = "hashline";
        cfg.agents.emplace("default", AgentConfig{
                                          .provider = "openai",
                                          .model = "gpt-test",
                                          .base_url = "https://example.test",
                                          .api_key = "test-key",
                                          .system_prompt = "You are a test agent.",
                                          .workspace = harness.workspace_root().string(),
                                          .edit_mode = "hashline",
                                      });
        cfg.agents.emplace("coder", AgentConfig{
                                        .provider = "openai",
                                        .model = "gpt-coder",
                                        .base_url = "https://example.test",
                                        .api_key = "coder-key",
                                        .system_prompt = "You are a coder agent.",
                                        .workspace = harness.workspace_root().string(),
                                        .edit_mode = "search_replace",
                                    });

        const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
        expect((runtime_configs.has_value()) >> fatal);
        auto default_it = runtime_configs->find("default");
        expect((default_it != runtime_configs->end()) >> fatal);
        expect(default_it->second.edit_mode == "hashline");
        auto coder_it = runtime_configs->find("coder");
        expect((coder_it != runtime_configs->end()) >> fatal);
        expect(coder_it->second.edit_mode == "search_replace");
    };

    "build_effective_agents_adds_legacy_default_when_missing"_test = [] {
        BootstrapHarness harness;
        Config cfg;
        cfg.provider = "openai";
        cfg.model = "gpt-test";
        cfg.base_url = "https://example.test";
        cfg.api_key = "test-key";
        cfg.workspace = harness.workspace_root().string();

        const auto agents = app::detail::build_effective_agents(cfg);
        auto it = agents.find("default");
        expect((it != agents.end()) >> fatal);
        expect(it->second.model == "gpt-test");
        expect(it->second.workspace == harness.workspace_root().string());
    };

    "build_effective_agents_assigns_default_workspace_when_missing"_test = [] {
        BootstrapHarness harness;
        ScopedEnvVar home_env("HOME", harness.home_root().string());

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
        const auto expected = (harness.home_root() / ".orangutan" / "workspace" / "main").lexically_normal().string();

        expect(agents.count("default") == 1_ul);
        expect(agents.count("coder") == 1_ul);
        expect(agents.at("default").workspace == expected);
        expect(agents.at("coder").workspace == expected);
    };

    "build_agent_runtime_configs_adds_legacy_default_when_missing"_test = [] {
        BootstrapHarness harness;
        Config cfg;
        cfg.provider = "openai";
        cfg.model = "gpt-test";
        cfg.base_url = "https://example.test";
        cfg.api_key = "test-key";
        cfg.workspace = harness.workspace_root().string();

        const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
        expect((runtime_configs.has_value()) >> fatal);
        auto it = runtime_configs->find("default");
        expect((it != runtime_configs->end()) >> fatal);
        expect(it->second.agent_key == "default");
        expect(it->second.model == "gpt-test");
    };

    "build_agent_runtime_configs_assigns_default_workspace_when_missing"_test = [] {
        BootstrapHarness harness;
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        Config cfg;
        cfg.agents.emplace("default", AgentConfig{
                                          .provider = "openai",
                                          .model = "gpt-test",
                                          .base_url = "https://example.test",
                                          .api_key = "test-key",
                                      });

        const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
        expect((runtime_configs.has_value()) >> fatal);
        auto it = runtime_configs->find("default");
        expect((it != runtime_configs->end()) >> fatal);

        const auto expected = std::filesystem::weakly_canonical(harness.home_root() / ".orangutan" / "workspace" / "main").string();
        expect(it->second.workspace_root == expected);
        expect(std::filesystem::exists(expected));
        expect(std::filesystem::is_directory(expected));
    };

    "build_agent_runtime_configs_preserves_default_subagents"_test = [] {
        BootstrapHarness harness;
        Config cfg;
        cfg.agents.emplace("default", AgentConfig{
                                          .provider = "openai",
                                          .model = "gpt-test",
                                          .base_url = "https://example.test",
                                          .api_key = "test-key",
                                          .workspace = harness.workspace_root().string(),
                                          .subagents = {"coder"},
                                      });
        cfg.agents.emplace("coder", AgentConfig{
                                        .provider = "openai",
                                        .model = "gpt-coder",
                                        .base_url = "https://example.test",
                                        .api_key = "coder-key",
                                        .workspace = harness.workspace_root().string(),
                                    });

        const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
        expect((runtime_configs.has_value()) >> fatal);
        auto default_it = runtime_configs->find("default");
        expect((default_it != runtime_configs->end()) >> fatal);
        expect(default_it->second.allowed_child_agents.size() == 1_ul);
        expect(default_it->second.allowed_child_agents.front() == "coder");
    };

    "build_subagent_child_runtime_configs_propagates_edit_mode"_test = [] {
        BootstrapHarness harness;
        std::unordered_map<std::string, app::AgentRuntimeConfig> runtime_configs;
        runtime_configs.emplace("default", app::AgentRuntimeConfig{
                                               .agent_key = "default",
                                               .provider_name = "openai",
                                               .api_key = "test-key",
                                               .model = "gpt-test",
                                               .base_url = "https://example.test",
                                               .system_prompt = "You are a test agent.",
                                               .workspace_root = harness.workspace_root().string(),
                                               .edit_mode = "search_replace",
                                           });

        const auto child_configs = app::detail::build_subagent_child_runtime_configs(runtime_configs);
        auto it = child_configs.find("default");
        expect((it != child_configs.end()) >> fatal);
        expect(it->second.edit_mode == "search_replace");
    };

    "repl_runtime_lists_memory_tools_and_skills"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto cfg = Config::load_from(harness.config_path());
        const auto runtime_configs = app::detail::build_agent_runtime_configs(cfg, "");
        expect((runtime_configs.has_value()) >> fatal);

        const auto runtime_it = runtime_configs->find("default");
        expect((runtime_it != runtime_configs->end()) >> fatal);

        MemoryStore memory_store((harness.home_root() / ".orangutan" / "memory.db"));
        const auto identity = derive_cli_identity(runtime_it->second.workspace_root, runtime_it->second.agent_key);
        app::AppRuntime app_runtime((harness.home_root() / ".orangutan" / "automation.db"));
        auto runtime = build_agent_runtime(AgentRuntimeBuildInput{
            .provider_name = runtime_it->second.provider_name,
            .api_key = runtime_it->second.api_key,
            .model = runtime_it->second.model,
            .fallback_models = runtime_it->second.fallback_models,
            .base_url = runtime_it->second.base_url,
            .agent_key = runtime_it->second.agent_key,
            .system_prompt = runtime_it->second.system_prompt,
            .workspace_root = runtime_it->second.workspace_root,
            .edit_mode = runtime_it->second.edit_mode,
            .memory = runtime_it->second.memory,
            .permissions = runtime_it->second.permissions,
            .allowed_child_agents = runtime_it->second.allowed_child_agents,
            .identity = identity,
            .memory_store = &memory_store,
            .automation_runtime = &app_runtime.automation_runtime(),
            .custom_tools = cfg.custom_tools,
            .mcp_servers = cfg.mcp_servers,
            .skill_paths = cfg.skill_paths,
            .hook_paths = cfg.hook_paths,
        });

        const auto definitions = runtime.tools.definitions();
        expect(orangutan::testing::has_tool_named(definitions, "memory_list"));
        expect(orangutan::testing::has_tool_named(definitions, "task"));
        expect(orangutan::testing::has_tool_named(definitions, "heartbeat"));
        expect(orangutan::testing::has_tool_named(definitions, "inbox"));
        expect(runtime.skills_prompt.contains("workspace-skill"));
    };

    "web_mode_builds_and_attaches_real_bootstrap_runtime_dependencies"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--agent", "default", "--web", "--port", "0"});

        expect((result.exit_code == 0_i) >> fatal) << result.output;
        expect((inspection_capture.inspection().has_value()) >> fatal);
        const auto &inspection = *inspection_capture.inspection();
        expect(inspection.has_session_store);
        expect(inspection.has_memory_store);
        expect(inspection.has_subagent_run_store);
        expect(inspection.has_subagent_manager);
        expect(inspection.has_runtime_bundle);
        expect(inspection.has_runtime_agent);
        expect(inspection.attached_session_store);
        expect(inspection.attached_tool_registry);
        expect(inspection.attached_skill_loader);
        expect(orangutan::testing::has_tool_named(inspection.tool_definitions, "memory_list"));
        expect(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")) != inspection.active_skill_names.end());
    };

    "web_mode_creates_web_assembly_dependencies_without_api_key"_test = [] {
        BootstrapHarness harness;
        harness.write_config_with_api_key("");
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--web", "--agent", "default", "--port", "0"});

        expect((result.exit_code == 0_i) >> fatal) << result.output;
        expect((inspection_capture.inspection().has_value()) >> fatal);
        const auto &inspection = *inspection_capture.inspection();
        expect(inspection.has_session_store);
        expect(inspection.has_memory_store);
        expect(inspection.has_subagent_run_store);
        expect(inspection.has_subagent_manager);
        expect(not inspection.has_runtime_bundle);
        expect(not inspection.has_runtime_agent);
        expect(inspection.attached_session_store);
        expect(not inspection.attached_tool_registry);
        expect(inspection.attached_skill_loader);
        expect(inspection.tool_definitions.empty());
        expect(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")) != inspection.active_skill_names.end());
    };

    "web_mode_starts_admin_ui_when_runtime_assembly_fails"_test = [] {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;
        ScopedWebRuntimeBuildFailureInjection build_failure("injected web runtime failure");

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--web", "--agent", "default", "--port", "0"});

        expect((result.exit_code == 0_i) >> fatal) << result.output;
        expect((inspection_capture.inspection().has_value()) >> fatal);
        const auto &inspection = *inspection_capture.inspection();
        expect(inspection.has_session_store);
        expect(inspection.has_memory_store);
        expect(inspection.has_subagent_run_store);
        expect(inspection.has_subagent_manager);
        expect(not inspection.has_runtime_bundle);
        expect(not inspection.has_runtime_agent);
        expect(inspection.attached_session_store);
        expect(not inspection.attached_tool_registry);
        expect(inspection.attached_skill_loader);
        expect(inspection.tool_definitions.empty());
        expect(std::ranges::find(inspection.active_skill_names, std::string("workspace-skill")) != inspection.active_skill_names.end());
        expect(inspection.runtime_build_error.contains("injected web runtime failure"));
    };

    "run_bootstrap_loads_protected_config_with_cli_password"_test = [] {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "cli-password", "agents.api_key");
        harness.write_config_with_api_key(protected_key);
        harness.create_sessions();
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto result = BootstrapHarness::invoke_bootstrap({
            "orangutan",
            "--cli",
            "--agent",
            "default",
            "--config-password",
            "cli-password",
            "--resume",
            "--event-stream",
            "--dump-session",
        });

        expect(result.exit_code == 0_i);
    };

    "run_bootstrap_loads_protected_config_with_environment_password_headless"_test = [] {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "env-password", "agents.api_key");
        harness.write_config_with_api_key(protected_key);
        harness.create_sessions();
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedEnvVar password_env("ORANGUTAN_CONFIG_PASSWORD", "env-password");

        const auto result = BootstrapHarness::invoke_bootstrap({
            "orangutan",
            "--cli",
            "--agent",
            "default",
            "--resume",
            "--event-stream",
            "--dump-session",
        });

        expect(result.exit_code == 0_i);
    };

    "run_bootstrap_fails_without_password_for_protected_config_headless"_test = [] {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "missing-password", "agents.api_key");
        harness.write_config_with_api_key(protected_key);
        harness.create_sessions();
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto result = BootstrapHarness::invoke_bootstrap({
            "orangutan",
            "--cli",
            "--agent",
            "default",
            "--resume",
            "--event-stream",
            "--dump-session",
        });

        expect(result.exit_code == 1_i);
        expect(result.output.contains("--config-password"));
        expect(result.output.contains("ORANGUTAN_CONFIG_PASSWORD"));
    };
};

} // namespace
