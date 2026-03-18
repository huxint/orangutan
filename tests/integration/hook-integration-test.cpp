#include "features/agent/agent-loop.hpp"
#include "features/hooks/hook-manager.hpp"
#include "core/tools/tool.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace orangutan;

namespace {

std::filesystem::path fixture_root() {
    return std::filesystem::path(SOURCE_DIR) / "tests/fixtures/hooks";
}

class MemorySink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    [[nodiscard]]
    std::vector<std::string> lines() const {
        std::scoped_lock lock(lines_mutex_);
        return lines_;
    }

protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::scoped_lock lock(lines_mutex_);
        lines_.emplace_back(formatted.data(), formatted.size());
    }

    void flush_() override {}

private:
    mutable std::mutex lines_mutex_;
    std::vector<std::string> lines_;
};

class ScopedDefaultLogger {
public:
    explicit ScopedDefaultLogger(const std::shared_ptr<MemorySink> &sink)
    : previous_(spdlog::default_logger()),
      previous_level_(spdlog::get_level()) {
        logger_ = std::make_shared<spdlog::logger>("hook-integration-test", sink);
        logger_->set_pattern("%l %v");
        spdlog::set_default_logger(logger_);
        spdlog::set_level(spdlog::level::debug);
    }

    ~ScopedDefaultLogger() {
        spdlog::set_default_logger(previous_);
        spdlog::set_level(previous_level_);
    }

    ScopedDefaultLogger(const ScopedDefaultLogger &) = delete;
    ScopedDefaultLogger &operator=(const ScopedDefaultLogger &) = delete;
    ScopedDefaultLogger(ScopedDefaultLogger &&) = delete;
    ScopedDefaultLogger &operator=(ScopedDefaultLogger &&) = delete;

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> previous_;
    spdlog::level::level_enum previous_level_;
};

class ToolCallingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used");
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        if (response_index_ == 0) {
            ++response_index_;
            return {
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "tool-1", .name = "demo", .input = json{{"value", "from-provider"}}}},
            };
        }
        if (response_index_ == 1) {
            ++response_index_;
            return {
                .stop_reason = "end_turn",
                .content = {TextBlock{.text = "final reply"}},
            };
        }
        throw std::runtime_error("no more responses queued");
    }

    std::string name() const override {
        return "hook-integration-provider";
    }

private:
    size_t response_index_ = 0;
};

struct AgentRunResult {
    std::string reply;
    std::optional<ToolResultBlock> tool_result;
    int execution_count = 0;
};

class HookIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = std::filesystem::temp_directory_path() / "orangutan_hook_integration_test";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(temp_root_);
        sink_ = std::make_shared<MemorySink>();
        logger_ = std::make_unique<ScopedDefaultLogger>(sink_);
    }

    void TearDown() override {
        logger_.reset();
        std::filesystem::remove_all(temp_root_);
    }

    [[nodiscard]]
    std::filesystem::path hook_root() const {
        return temp_root_ / "hooks";
    }

    void copy_fixture_script(const std::filesystem::path &relative_path) const {
        const auto destination = hook_root() / relative_path;
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(fixture_root() / relative_path, destination, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(destination,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    void write_hook_script(const std::filesystem::path &relative_path, const std::string &body) const {
        const auto destination = hook_root() / relative_path;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream out(destination);
        out << body;
        out.close();
        std::filesystem::permissions(destination,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    [[nodiscard]]
    AgentRunResult run_agent() const {
        HookManager manager;
        manager.load_from_directories({hook_root().string()});

        ToolCallingProvider provider;
        ToolRegistry tools;
        int execution_count = 0;
        tools.register_tool(Tool{
            .definition =
                ToolDef{
                    .name = "demo",
                    .description = "demo tool",
                    .input_schema = json{{"type", "object"}},
                },
            .execute =
                [&execution_count](const json &) {
                    ++execution_count;
                    return std::string{"tool output"};
                },
        });

        AgentLoop loop(provider, tools, "test system prompt", nullptr, {}, &manager);

        std::optional<ToolResultBlock> tool_result;
        const auto reply = loop.run("please run the demo tool", {}, [&tool_result](const std::string &event_type, const ToolUseBlock &, const ToolResultBlock *result) {
            if (event_type == "tool_finished" && result != nullptr) {
                tool_result = *result;
            }
        });

        return {
            .reply = reply,
            .tool_result = std::move(tool_result),
            .execution_count = execution_count,
        };
    }

    [[nodiscard]]
    bool contains_log(std::string_view needle) const {
        return std::ranges::any_of(sink_->lines(), [needle](const std::string &line) {
            return line.find(needle) != std::string::npos;
        });
    }

    [[nodiscard]]
    const std::filesystem::path &temp_root() const {
        return temp_root_;
    }

private:
    std::filesystem::path temp_root_;
    std::shared_ptr<MemorySink> sink_;
    std::unique_ptr<ScopedDefaultLogger> logger_;
};

} // namespace

TEST_F(HookIntegrationTest, BeforeToolCallBlockShortCircuitsAndSkipsToolExecution) {
    copy_fixture_script("before_tool_call/02-block.sh");
    const auto marker = temp_root() / "unexpected-hook-ran";
    write_hook_script("before_tool_call/03-should-not-run.sh", "#!/bin/sh\ntouch \"" + marker.string() + "\"\nexit 0\n");

    const auto result = run_agent();

    EXPECT_EQ(result.reply, "final reply");
    ASSERT_TRUE(result.tool_result.has_value());
    EXPECT_TRUE(result.tool_result->is_error);
    EXPECT_NE(result.tool_result->content.find("02-block.sh"), std::string::npos);
    EXPECT_NE(result.tool_result->content.find("blocked by hook"), std::string::npos);
    EXPECT_EQ(result.execution_count, 0);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(HookIntegrationTest, BeforeToolCallTimeoutBlocksToolAndLogsWarning) {
    copy_fixture_script("before_tool_call/03-timeout.sh");

    const auto result = run_agent();

    EXPECT_EQ(result.reply, "final reply");
    ASSERT_TRUE(result.tool_result.has_value());
    EXPECT_TRUE(result.tool_result->is_error);
    EXPECT_NE(result.tool_result->content.find("03-timeout.sh"), std::string::npos);
    EXPECT_EQ(result.execution_count, 0);
    EXPECT_TRUE(contains_log("Hook timed out after 5s"));
}

TEST_F(HookIntegrationTest, AfterToolCallLogsStderrWithoutChangingToolResult) {
    copy_fixture_script("after_tool_call/01-log.sh");

    const auto result = run_agent();

    EXPECT_EQ(result.reply, "final reply");
    ASSERT_TRUE(result.tool_result.has_value());
    EXPECT_FALSE(result.tool_result->is_error);
    EXPECT_EQ(result.tool_result->content, "tool output");
    EXPECT_EQ(result.execution_count, 1);
    EXPECT_TRUE(contains_log("[01-log.sh] after hook stderr"));
}

TEST_F(HookIntegrationTest, ToolHookJsonValidationFixturesReceiveExpectedContext) {
    copy_fixture_script("before_tool_call/04-validate-json.py");
    copy_fixture_script("after_tool_call/02-validate-json.py");

    const auto result = run_agent();

    EXPECT_EQ(result.reply, "final reply");
    ASSERT_TRUE(result.tool_result.has_value());
    EXPECT_FALSE(result.tool_result->is_error);
    EXPECT_EQ(result.tool_result->content, "tool output");
    EXPECT_EQ(result.execution_count, 1);
    EXPECT_TRUE(contains_log("validated before_tool_call"));
    EXPECT_TRUE(contains_log("validated after_tool_call"));
}

TEST_F(HookIntegrationTest, SessionDispatchHelpersProvideExpectedContext) {
    copy_fixture_script("session_start/01-validate-json.py");
    copy_fixture_script("session_end/01-validate-json.py");

    HookManager manager;
    manager.load_from_directories({hook_root().string()});

    dispatch_session_start(&manager, "session-123", 3);
    dispatch_session_end(&manager, "session-123", 7);

    EXPECT_TRUE(contains_log("validated session_start"));
    EXPECT_TRUE(contains_log("validated session_end"));
}
