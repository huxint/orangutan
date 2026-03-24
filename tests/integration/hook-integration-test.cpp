#include "features/agent/agent-loop.hpp"
#include "features/hooks/hook-manager.hpp"
#include "core/tools/tool.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include "support/ut.hpp"
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

using ScopedDefaultLogger = orangutan::testing::ScopedDefaultLogger<MemorySink>;

class ToolCallingProvider final : public Provider {
public:
    LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used");
    }

    LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
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

class HookIntegrationHarness {
public:
    HookIntegrationHarness()
    : temp_root_(orangutan::testing::unique_test_root("hook-integration")),
      sink_(std::make_shared<MemorySink>()),
      logger_("hook-integration-test", sink_) {}

    [[nodiscard]]
    std::filesystem::path hook_root() const {
        return temp_root_ / "hooks";
    }

    void copy_fixture_script(const std::filesystem::path &relative_path) const {
        const auto destination = hook_root() / relative_path;
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(fixture_root() / relative_path, destination, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(destination, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);
    }

    void write_hook_script(const std::filesystem::path &relative_path, const std::string &body) const {
        const auto destination = hook_root() / relative_path;
        std::filesystem::create_directories(destination.parent_path());
        std::ofstream out(destination);
        out << body;
        out.close();
        std::filesystem::permissions(destination, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
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
    ScopedDefaultLogger logger_;
};

boost::ut::suite hook_integration_suite = [] {
    using namespace boost::ut;

    "before_tool_call_block_short_circuits_and_skips_tool_execution"_test = [] {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/02-block.sh");
        const auto marker = harness.temp_root() / "unexpected-hook-ran";
        harness.write_hook_script("before_tool_call/03-should-not-run.sh", "#!/bin/sh\ntouch \"" + marker.string() + "\"\nexit 0\n");

        const auto result = harness.run_agent();

        expect(result.reply == "final reply");
        expect(result.tool_result.has_value() >> fatal) << "expected tool result after hook short-circuit";
        expect(result.tool_result->is_error);
        expect(result.tool_result->content.find("02-block.sh") != std::string::npos);
        expect(result.tool_result->content.find("blocked by hook") != std::string::npos);
        expect(result.execution_count == 0_i);
        expect(not std::filesystem::exists(marker));
    };

    "before_tool_call_timeout_blocks_tool_and_logs_warning"_test = [] {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/03-timeout.sh");

        const auto result = harness.run_agent();

        expect(result.reply == "final reply");
        expect(result.tool_result.has_value() >> fatal) << "expected tool result after timeout";
        expect(result.tool_result->is_error);
        expect(result.tool_result->content.find("03-timeout.sh") != std::string::npos);
        expect(result.execution_count == 0_i);
        expect(harness.contains_log("Hook timed out after 5s"));
    };

    "after_tool_call_logs_stderr_without_changing_tool_result"_test = [] {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("after_tool_call/01-log.sh");

        const auto result = harness.run_agent();

        expect(result.reply == "final reply");
        expect(result.tool_result.has_value() >> fatal) << "expected tool result after after_tool_call hook";
        expect(not result.tool_result->is_error);
        expect(result.tool_result->content == "tool output");
        expect(result.execution_count == 1_i);
        expect(harness.contains_log("[01-log.sh] after hook stderr"));
    };

    "tool_hook_json_validation_fixtures_receive_expected_context"_test = [] {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/04-validate-json.py");
        harness.copy_fixture_script("after_tool_call/02-validate-json.py");

        const auto result = harness.run_agent();

        expect(result.reply == "final reply");
        expect(result.tool_result.has_value() >> fatal) << "expected tool result after validation hooks";
        expect(not result.tool_result->is_error);
        expect(result.tool_result->content == "tool output");
        expect(result.execution_count == 1_i);
        expect(harness.contains_log("validated before_tool_call"));
        expect(harness.contains_log("validated after_tool_call"));
    };

    "session_dispatch_helpers_provide_expected_context"_test = [] {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("session_start/01-validate-json.py");
        harness.copy_fixture_script("session_end/01-validate-json.py");

        HookManager manager;
        manager.load_from_directories({harness.hook_root().string()});

        dispatch_session_start(&manager, "session-123", 3);
        dispatch_session_end(&manager, "session-123", 7);

        expect(harness.contains_log("validated session_start"));
        expect(harness.contains_log("validated session_end"));
    };
};

} // namespace
