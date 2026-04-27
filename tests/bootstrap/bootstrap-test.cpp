#include "bootstrap/bootstrap.hpp"
#include "bootstrap/channel-serve.hpp"
#include "bootstrap/agent-runtime.hpp"
#include "bootstrap/app-runtime.hpp"
#include "bootstrap/runtime-control.hpp"
#include "bootstrap/identity.hpp"
#include "memory/memory-store.hpp"
#include "web/web-server.hpp"
#include "config/config.hpp"
#include "config/secret-protection.hpp"
#include "storage/session-store.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <vector>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

namespace {

    namespace bootstrap = orangutan::bootstrap;
    namespace bootstrap_detail = orangutan::bootstrap::detail;

    ProfileConfig make_profile(std::initializer_list<std::pair<const std::string, ModelConfig>> models, std::string api_key = "test-key",
                               std::string base_url = "https://example.test") {
        ProfileConfig profile{
            .base_url = std::move(base_url),
            .api_key = std::move(api_key),
        };
        for (const auto &[name, model] : models) {
            profile.models.emplace(name, model);
        }
        return profile;
    }

    AgentConfig make_agent(std::string profile, std::string model, std::string workspace) {
        return AgentConfig{
            .profile = std::move(profile),
            .model = std::move(model),
            .workspace = std::move(workspace),
        };
    }
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

            std::size_t written_total = 0;
            while (written_total < content.size()) {
                const auto remaining = content.substr(written_total);
                const auto written = ::write(fds_[1], remaining.data(), remaining.size());
                if (written <= 0) {
                    throw std::runtime_error("pipe write failed");
                }
                written_total += static_cast<std::size_t>(written);
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
            return home_root_ / ".orangutan" / "config.json";
        }

        [[nodiscard]]
        std::filesystem::path session_db_path() const {
            return workspace_root_ / ".orangutan" / "sessions.db";
        }

        void write_config() const {
            write_config_with_api_key("test-key");
        }

        void write_config_with_api_key(const std::string &api_key) const {
            std::ofstream out(config_path());
            out << "{\n";
            out << "  \"profiles\": {\n";
            out << "    \"test-profile\": {\n";
            out << "      \"base_url\": \"https://example.test\",\n";
            out << R"(      "api_key": ")" << api_key << "\",\n";
            out << "      \"models\": {\n";
            out << "        \"gpt-test\": {\n";
            out << "          \"provider\": \"openai\",\n";
            out << "          \"protocol\": \"chat-completions\"\n";
            out << "        }\n";
            out << "      }\n";
            out << "    }\n";
            out << "  },\n";
            out << "  \"agents\": {\n";
            out << "    \"default\": {\n";
            out << "      \"profile\": \"test-profile\",\n";
            out << "      \"model\": \"gpt-test\",\n";
            out << R"(      "workspace": ")" << workspace_root_.string() << "\"\n";
            out << "    }\n";
            out << "  }\n";
            out << "}\n";
        }

        static void write_skill(const std::filesystem::path &base_dir, const std::string &dir_name, const std::string &skill_name, const std::string &body) {
            const auto skill_dir = base_dir / dir_name;
            std::filesystem::create_directories(skill_dir);
            std::ofstream out(skill_dir / "SKILL.md");
            out << "---\n";
            out << "name: " << skill_name << "\n";
            out << "description: bootstrap test skill\n";
            out << "---\n\n";
            out << body << "\n";
        }

        void create_sessions() const {
            SessionStore session_store(session_db_path());
            const auto cli_identity = bootstrap::derive_cli_identity(workspace_root_.string(), "default");
            const std::vector<Message> history_a{
                Message(base::role::user, {Text{"first"}}),
                Message(base::role::assistant, {Text{"reply"}}),
            };
            const std::vector<Message> history_b{
                Message(base::role::user, {Text{"second"}}),
                Message(base::role::assistant, {Text{"reply two"}}),
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

                exit_code = bootstrap::run(static_cast<int>(argv.size() - 1), argv.data());
            }

            std::string output;
            std::array<char, 256> buffer{};
            std::size_t read_bytes = 0;
            while ((read_bytes = ::read(output_pipe.read_end(), buffer.data(), buffer.size())) > 0) {
                output.append(buffer.data(), read_bytes);
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

    TEST_CASE("resume_without_explicit_id_does_not_consume_piped_message_input") {
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

            CHECK(bootstrap::run(static_cast<int>(argv.size() - 1), argv.data()) == 0);
        }

        output_pipe.close_read();
    };

    TEST_CASE("run_bootstrap_requires_at_least_one_entry_flag") {
        BootstrapHarness harness;
        harness.write_config();
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--agent", "default"});

        CHECK(result.exit_code == 1);
        CHECK(result.output.contains("specify at least one entry flag"));
    };

    TEST_CASE("channel_only_mode_returns_startup_failure_instead_of_hanging") {
        BootstrapHarness harness;
        harness.write_config();
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedChannelModeCallback channel_failure([] {
            return 1;
        });

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--channel"});

        CHECK(result.exit_code == 1);
    };

    TEST_CASE("build_effective_agents_does_not_synthesize_default_when_missing") {
        BootstrapHarness harness;
        Config cfg;
        cfg.profile = "shared";
        cfg.model = "gpt-test";
        cfg.workspace = harness.workspace_root().string();
        cfg.agents.emplace("coder", make_agent("shared", "gpt-coder", harness.workspace_root().string()));

        const auto agents = bootstrap::detail::build_effective_agents(cfg);
        CHECK_FALSE(agents.contains("default"));
        REQUIRE(agents.contains("coder"));
        CHECK(agents.at("coder").workspace == harness.workspace_root().string());
    };

    TEST_CASE("build_effective_agents_assigns_default_workspace_when_missing") {
        BootstrapHarness harness;
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        Config cfg;
        cfg.profile = "shared";
        cfg.model = "gpt-test";
        cfg.agents.emplace("coder", AgentConfig{
                                        .profile = "shared",
                                        .model = "gpt-coder",
                                    });

        const auto agents = bootstrap::detail::build_effective_agents(cfg);
        const auto expected = (harness.home_root() / "workspace").lexically_normal().string();

        CHECK_FALSE(agents.contains("default"));
        CHECK(agents.count("coder") == 1UL);
        CHECK(agents.at("coder").workspace == expected);
    };

    TEST_CASE("build_agent_runtime_configs_requires_explicit_default") {
        BootstrapHarness harness;
        Config cfg;
        cfg.profile = "shared";
        cfg.model = "gpt-test";
        cfg.workspace = harness.workspace_root().string();
        cfg.profiles.emplace("shared", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));

        const auto runtime_configs = bootstrap::detail::build_agent_runtime_configs(cfg, "");
        REQUIRE(runtime_configs.has_value());
        CHECK_FALSE(runtime_configs->contains("default"));
    };

    TEST_CASE("build_agent_runtime_configs_assigns_default_workspace_when_missing") {
        BootstrapHarness harness;
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        Config cfg;
        cfg.profiles.emplace("shared", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
        cfg.agents.emplace("default", AgentConfig{
                                          .profile = "shared",
                                          .model = "gpt-test",
                                      });

        const auto runtime_configs = bootstrap::detail::build_agent_runtime_configs(cfg, "");
        REQUIRE(runtime_configs.has_value());
        auto it = runtime_configs->find("default");
        REQUIRE(it != runtime_configs->end());

        const auto expected = std::filesystem::weakly_canonical(harness.home_root() / "workspace").string();
        CHECK(it->second.workspace_root == expected);
        CHECK(std::filesystem::exists(expected));
        CHECK(std::filesystem::is_directory(expected));
    };

    TEST_CASE("build_agent_runtime_configs_resolves_cross_profile_fallbacks") {
        BootstrapHarness harness;
        Config cfg;
        cfg.profiles.emplace("openai", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "responses"}}}, "openai-key",
                                                    "https://openai.example.test"));
        cfg.profiles.emplace("anthropic", make_profile({{"claude-test", ModelConfig{.provider = "anthropic", .protocol = "messages"}}}, "anthropic-key",
                                                       "https://anthropic.example.test"));
        cfg.agents.emplace("default", AgentConfig{
                                          .profile = "openai",
                                          .model = "gpt-test",
                                          .fallback_models = {FallbackModelRef{"anthropic", "claude-test"}},
                                          .workspace = harness.workspace_root().string(),
                                      });

        const auto runtime_configs = bootstrap::detail::build_agent_runtime_configs(cfg, "");
        REQUIRE(runtime_configs.has_value());
        const auto it = runtime_configs->find("default");
        REQUIRE(it != runtime_configs->end());
        REQUIRE(it->second.provider_route.fallbacks.size() == 1UL);
        CHECK(it->second.fallback_models == std::vector<std::string>{"anthropic:claude-test"});
        CHECK(it->second.provider_route.fallbacks[0].profile_name == "anthropic");
        CHECK(it->second.provider_route.fallbacks[0].provider == providers::provider_kind::anthropic);
        CHECK(it->second.provider_route.fallbacks[0].protocol == providers::protocol_kind::messages);
        CHECK(it->second.provider_route.fallbacks[0].model == "claude-test");
        CHECK(it->second.provider_route.fallbacks[0].base_url == "https://anthropic.example.test");
        CHECK(it->second.provider_route.fallbacks[0].api_key == "anthropic-key");
    };

    TEST_CASE("build_agent_runtime_configs_preserves_cli_api_key_override") {
        BootstrapHarness harness;
        Config cfg;
        cfg.profiles.emplace("openai", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "responses"}}}, "profile-key"));
        cfg.agents.emplace("default", AgentConfig{
                                          .profile = "openai",
                                          .model = "gpt-test",
                                          .workspace = harness.workspace_root().string(),
                                      });

        const auto runtime_configs = bootstrap::detail::build_agent_runtime_configs(cfg, "cli-key");
        REQUIRE(runtime_configs.has_value());
        const auto it = runtime_configs->find("default");
        REQUIRE(it != runtime_configs->end());

        CHECK(it->second.api_key_override == "cli-key");
        CHECK(it->second.provider_route.primary.api_key == "cli-key");
    };

    TEST_CASE("build_agent_runtime_configs_constructs_permission_contexts") {
        BootstrapHarness harness;
        Config cfg;
        cfg.permissions_config = PermissionConfig{
            .default_mode = permission_mode::accept_edits,
            .allow = {"read"},
        };
        cfg.profiles.emplace("shared", make_profile({{"gpt-test", ModelConfig{.provider = "openai", .protocol = "chat-completions"}}}));
        cfg.agents.emplace("default", AgentConfig{
                                          .profile = "shared",
                                          .model = "gpt-test",
                                          .workspace = harness.workspace_root().string(),
                                      });

        const auto runtime_configs =
            bootstrap::detail::build_agent_runtime_configs(cfg, "", CLIPermissionOptions{.mode_override = permission_mode::plan, .allowed_tools = {"task(list)"}});
        REQUIRE(runtime_configs.has_value());
        const auto it = runtime_configs->find("default");
        REQUIRE(it != runtime_configs->end());

        CHECK(it->second.permission_context.mode == permission_mode::plan);
        CHECK(std::ranges::any_of(it->second.permission_context.allow_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "read" && rule.source == permission_rule_source::user_settings;
        }));
        CHECK(std::ranges::any_of(it->second.permission_context.allow_rules, [](const PermissionRule &rule) {
            return rule.tool_name == "task" && rule.source == permission_rule_source::cli_arg && rule.content.has_value() && rule.content->pattern == "list";
        }));
    };

    TEST_CASE("repl_runtime_lists_memory_tools_and_skills") {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());

        const auto cfg = Config::load_from(harness.config_path());
        const auto runtime_configs = bootstrap::detail::build_agent_runtime_configs(cfg, "");
        REQUIRE(runtime_configs.has_value());

        const auto runtime_it = runtime_configs->find("default");
        REQUIRE(runtime_it != runtime_configs->end());

        MemoryStore memory_store((harness.workspace_root() / ".orangutan" / "memory.db"));
        const auto identity = bootstrap::derive_cli_identity(runtime_it->second.workspace_root, runtime_it->second.agent_key);
        bootstrap::AppRuntime app_runtime((harness.workspace_root() / ".orangutan" / "automation.db"));
        auto runtime = bootstrap::build_agent_runtime(bootstrap::AgentRuntimeBuildInput{
            .provider_route = runtime_it->second.provider_route,
            .agent_key = runtime_it->second.agent_key,
            .workspace_root = runtime_it->second.workspace_root,
            .permission_context = runtime_it->second.permission_context,
            .identity = identity,
            .memory_store = &memory_store,
            .automation_runtime = &app_runtime.automation_runtime(),
            .custom_tools = cfg.custom_tools,
            .mcp_servers = cfg.mcp_servers,
            .skill_paths = cfg.skill_paths,
            .hook_paths = cfg.hook_paths,
        });

        const auto definitions = runtime.tools().definitions();
        CHECK(not(orangutan::testing::has_tool_named(definitions, "task")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "heartbeat")));
        CHECK(not(orangutan::testing::has_tool_named(definitions, "inbox")));
        CHECK(orangutan::testing::has_tool_named(definitions, "tool_search"));
        CHECK(runtime.skills_prompt.contains("workspace-skill"));
    };

    TEST_CASE("web_mode_builds_and_attaches_real_bootstrap_runtime_dependencies") {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--agent", "default", "--web", "--port", "0"});

        INFO(result.output);
        REQUIRE(result.exit_code == 0);
        REQUIRE(inspection_capture.inspection().has_value());
        const auto &inspection = *inspection_capture.inspection();
        CHECK(inspection.has_session_store);
        CHECK(inspection.has_memory_store);
        CHECK(inspection.has_runtime_bundle);
        CHECK(inspection.has_runtime_agent);
        CHECK(inspection.attached_session_store);
        CHECK(inspection.attached_tool_registry);
        CHECK(inspection.attached_skill_loader);
        CHECK(inspection.attached_config_save_path);
        CHECK(orangutan::testing::has_tool_named(inspection.tool_definitions, "tool_search"));
        CHECK(std::ranges::find(inspection.active_skill_names, std::string{"workspace-skill"}) != inspection.active_skill_names.end());
    };

    TEST_CASE("web_mode_creates_web_assembly_dependencies_without_api_key") {
        BootstrapHarness harness;
        harness.write_config_with_api_key("");
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--web", "--agent", "default", "--port", "0"});

        INFO(result.output);
        REQUIRE(result.exit_code == 0);
        REQUIRE(inspection_capture.inspection().has_value());
        const auto &inspection = *inspection_capture.inspection();
        CHECK(inspection.has_session_store);
        CHECK(inspection.has_memory_store);
        CHECK_FALSE(inspection.has_runtime_bundle);
        CHECK_FALSE(inspection.has_runtime_agent);
        CHECK(inspection.attached_session_store);
        CHECK_FALSE(inspection.attached_tool_registry);
        CHECK(inspection.attached_skill_loader);
        CHECK(inspection.attached_config_save_path);
        CHECK(inspection.tool_definitions.empty());
        CHECK(std::ranges::find(inspection.active_skill_names, std::string{"workspace-skill"}) != inspection.active_skill_names.end());
    };

    TEST_CASE("web_mode_starts_admin_ui_when_runtime_assembly_fails") {
        BootstrapHarness harness;
        harness.write_config();
        BootstrapHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");
        ScopedEnvVar home_env("HOME", harness.home_root().string());
        ScopedUnsetEnvVar anthropic_api_key_env("ANTHROPIC_API_KEY");
        ScopedUnsetEnvVar llm_api_key_env("LLM_API_KEY");
        ScopedWebStartupInspectionCapture inspection_capture;
        ScopedWebRuntimeBuildFailureInjection build_failure("injected web runtime failure");

        const auto result = BootstrapHarness::invoke_bootstrap({"orangutan", "--web", "--agent", "default", "--port", "0"});

        INFO(result.output);
        REQUIRE(result.exit_code == 0);
        REQUIRE(inspection_capture.inspection().has_value());
        const auto &inspection = *inspection_capture.inspection();
        CHECK(inspection.has_session_store);
        CHECK(inspection.has_memory_store);
        CHECK_FALSE(inspection.has_runtime_bundle);
        CHECK_FALSE(inspection.has_runtime_agent);
        CHECK(inspection.attached_session_store);
        CHECK_FALSE(inspection.attached_tool_registry);
        CHECK(inspection.attached_skill_loader);
        CHECK(inspection.attached_config_save_path);
        CHECK(inspection.tool_definitions.empty());
        CHECK(std::ranges::find(inspection.active_skill_names, std::string{"workspace-skill"}) != inspection.active_skill_names.end());
        CHECK(inspection.runtime_build_error.contains("injected web runtime failure"));
    };

    TEST_CASE("run_bootstrap_loads_protected_config_with_cli_password") {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "cli-password", "profiles.api_key");
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

        CHECK(result.exit_code == 0);
    };

    TEST_CASE("run_bootstrap_loads_protected_config_with_environment_password_headless") {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "env-password", "profiles.api_key");
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

        CHECK(result.exit_code == 0);
    };

    TEST_CASE("run_bootstrap_fails_without_password_for_protected_config_headless") {
        BootstrapHarness harness;
        const auto protected_key = protect_config_secret("test-key", "missing-password", "profiles.api_key");
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

        CHECK(result.exit_code == 1);
        CHECK(result.output.contains("--config-password"));
        CHECK(result.output.contains("ORANGUTAN_CONFIG_PASSWORD"));
    };

} // namespace
