#include "agent/agent-loop.hpp"
#include "hooks/hook-manager.hpp"
#include "test-helpers.hpp"
#include "test-provider-support.hpp"
#include "tools/registry/tool.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
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

    class ToolCallingProvider {
    public:
        ToolCallingProvider()
        : backend_(testing::make_fake_provider_backend([this](const providers::ProviderRoute &route, const providers::ProviderRequest &,
                                                              const providers::ProviderEventSink &) {
              if (response_index_ == 0) {
                  ++response_index_;
                  return providers::ProviderResult{
                      .response =
                          LLMResponse{
                              .stop_reason = response_stop_reason::tool_use,
                              .content = {ToolUse("tool-1", "demo", nlohmann::json{{"value", "from-provider"}})},
                          },
                      .usage_snapshot = {},
                      .active_target = route.primary,
                  };
              }
              if (response_index_ == 1) {
                  ++response_index_;
                  return providers::ProviderResult{
                      .response = testing::make_text_response("final reply"),
                      .usage_snapshot = {},
                      .active_target = route.primary,
                  };
              }
              throw std::runtime_error("no more responses queued");
          })),
          system(backend_),
          route(testing::make_test_route("test-model")) {
            backend_->set_label("hook-integration-provider");
        }

        std::shared_ptr<testing::FakeProviderBackend> backend_;
        std::size_t response_index_ = 0;
        providers::ProviderSystem system;
        providers::ProviderRoute route;
    };

    struct AgentRunResult {
        std::string reply;
        std::optional<ToolResult> tool_result;
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
                        .input_schema = nlohmann::json{{"type", "object"}},
                    },
                .execute =
                    [&execution_count](const nlohmann::json &) {
                        ++execution_count;
                        return std::string{"tool output"};
                    },
            });

            AgentLoop loop(provider.system, provider.route, tools, nullptr, {}, &manager);

            std::optional<ToolResult> tool_result;
            const auto reply = loop.run("please run the demo tool", {}, [&tool_result](const std::string &event_type, const ToolUse &, const ToolResult *result) {
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
                return line.contains(needle);
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

    TEST_CASE("before_tool_call_block_short_circuits_and_skips_tool_execution") {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/02-block.sh");
        const auto marker = harness.temp_root() / "unexpected-hook-ran";
        harness.write_hook_script("before_tool_call/03-should-not-run.sh", "#!/bin/sh\ntouch \"" + marker.string() + "\"\nexit 0\n");

        const auto result = harness.run_agent();

        CHECK(result.reply == "final reply");
        INFO("expected tool result after hook short-circuit");
        REQUIRE(result.tool_result.has_value());
        CHECK(result.tool_result->is_error);
        CHECK(result.tool_result->content.contains("02-block.sh"));
        CHECK(result.tool_result->content.contains("blocked by hook"));
        CHECK(result.execution_count == 0);
        CHECK_FALSE(std::filesystem::exists(marker));
    };

    TEST_CASE("before_tool_call_timeout_blocks_tool_and_logs_warning") {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/03-timeout.sh");

        const auto result = harness.run_agent();

        CHECK(result.reply == "final reply");
        INFO("expected tool result after timeout");
        REQUIRE(result.tool_result.has_value());
        CHECK(result.tool_result->is_error);
        CHECK(result.tool_result->content.contains("03-timeout.sh"));
        CHECK(result.execution_count == 0);
        CHECK(harness.contains_log("hook timed out after 5s"));
    };

    TEST_CASE("after_tool_call_logs_stderr_without_changing_tool_result") {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("after_tool_call/01-log.sh");

        const auto result = harness.run_agent();

        CHECK(result.reply == "final reply");
        INFO("expected tool result after after_tool_call hook");
        REQUIRE(result.tool_result.has_value());
        CHECK_FALSE(result.tool_result->is_error);
        CHECK(result.tool_result->content == "tool output");
        CHECK(result.execution_count == 1);
        CHECK(harness.contains_log("[01-log.sh] after hook stderr"));
    };

    TEST_CASE("tool_hook_json_validation_fixtures_receive_expected_context") {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("before_tool_call/04-validate-json.py");
        harness.copy_fixture_script("after_tool_call/02-validate-json.py");

        const auto result = harness.run_agent();

        CHECK(result.reply == "final reply");
        INFO("expected tool result after validation hooks");
        REQUIRE(result.tool_result.has_value());
        CHECK_FALSE(result.tool_result->is_error);
        CHECK(result.tool_result->content == "tool output");
        CHECK(result.execution_count == 1);
        CHECK(harness.contains_log("validated before_tool_call"));
        CHECK(harness.contains_log("validated after_tool_call"));
    };

    TEST_CASE("session_dispatch_helpers_provide_expected_context") {
        HookIntegrationHarness harness;
        harness.copy_fixture_script("session_start/01-validate-json.py");
        harness.copy_fixture_script("session_end/01-validate-json.py");

        HookManager manager;
        manager.load_from_directories({harness.hook_root().string()});

        dispatch_session_start(&manager, "session-123", 3);
        dispatch_session_end(&manager, "session-123", 7);

        CHECK(harness.contains_log("validated session_start"));
        CHECK(harness.contains_log("validated session_end"));
    };

} // namespace
