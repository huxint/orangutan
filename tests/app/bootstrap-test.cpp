#include "app/bootstrap.hpp"

#include "app/runtime/identity.hpp"
#include "infra/storage/session-store.hpp"
#include "test-helpers.hpp"

#include <array>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

using namespace orangutan;
using orangutan::testing::ScopedEnvVar;

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
        std::ofstream out(config_path());
        out << "[agents.default]\n";
        out << "provider = \"openai\"\n";
        out << "model = \"gpt-test\"\n";
        out << "base_url = \"https://example.test\"\n";
        out << "api_key = \"test-key\"\n";
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
            "orangutan",
            "--agent",
            "default",
            "--resume",
            "--event-stream",
            "--dump-session",
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

} // namespace
