#include "core/tools/tool.hpp"
#include "features/automation/runtime.hpp"
#include "features/automation/store.hpp"
#include "features/tools/core/background-completion.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace orangutan;

namespace {

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

const ToolDef *find_tool(const std::vector<ToolDef> &definitions, const std::string &name) {
    const auto it = std::ranges::find_if(definitions, [&](const ToolDef &definition) {
        return definition.name == name;
    });
    return it == definitions.end() ? nullptr : &(*it);
}

const orangutan::automation::InboxItem *find_inbox_item_by_body_type(const std::vector<orangutan::automation::InboxItem> &items, const std::string &type) {
    const auto it = std::ranges::find_if(items, [&](const orangutan::automation::InboxItem &item) {
        return json::parse(item.body).value("type", "") == type;
    });
    return it == items.end() ? nullptr : &(*it);
}

class BackgroundShellCompletionTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", orangutan::testing::test_tmp_root().string());
        workspace_root_ = orangutan::testing::test_tmp_root() / "background-shell-completion-workspace";
        const auto db_path = orangutan::testing::test_tmp_root() / "background-shell-completion.db";
        std::filesystem::remove_all(workspace_root_);
        std::filesystem::remove(db_path);
        std::filesystem::create_directories(workspace_root_);

        store_ = std::make_unique<orangutan::automation::Store>(db_path.string());
        runtime_ = std::make_unique<orangutan::automation::Runtime>(*store_);
        completion_owner_ = std::make_shared<int>(1);
        tool_context_ = ToolRuntimeContext{
            .runtime_key = "runtime:test:background-shell",
            .agent_key = "assistant",
            .scope_key = "scope:test",
            .automation_runtime = runtime_.get(),
        };
        tool_context_.background_completion_runtime = make_background_completion_runtime_bindings(completion_owner_, runtime_.get(), [this](const std::string &message) {
            std::scoped_lock lock(resume_messages_mutex_);
            resume_messages_.push_back(message);
            return std::optional<std::string>{};
        });

        register_builtin_tools(registry_, nullptr, workspace_root_.string(), &tool_context_);
    }

    void TearDown() override {
        completion_owner_.reset();
        tool_context_.invalidate_background_completion_runtime();
        runtime_.reset();
        store_.reset();
        std::filesystem::remove_all(workspace_root_);
    }

    json start_background_shell(json input) {
        input["background"] = true;
        const auto result = registry_.execute(ToolUseBlock{
            .id = "background-shell",
            .name = "shell",
            .input = std::move(input),
        });
        EXPECT_FALSE(result.is_error);
        if (result.is_error) {
            return {};
        }

        return json::parse(result.content);
    }

    std::vector<orangutan::automation::InboxItem> wait_for_inbox_size(size_t expected_size, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto items = runtime_->list_inbox(tool_context_.agent_key);
            if (items.size() >= expected_size) {
                return items;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        ADD_FAILURE() << "background completion inbox items did not arrive in time";
        return runtime_->list_inbox(tool_context_.agent_key);
    }

    std::vector<std::string> wait_for_resume_messages_size(size_t expected_size, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::scoped_lock lock(resume_messages_mutex_);
                if (resume_messages_.size() >= expected_size) {
                    return resume_messages_;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        ADD_FAILURE() << "background completion resume messages did not arrive in time";
        std::scoped_lock lock(resume_messages_mutex_);
        return resume_messages_;
    }

    ToolRegistry registry_;
    ToolRuntimeContext tool_context_;
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path workspace_root_;
    std::unique_ptr<orangutan::automation::Store> store_;
    std::unique_ptr<orangutan::automation::Runtime> runtime_;
    std::shared_ptr<int> completion_owner_;
    std::mutex resume_messages_mutex_;
    std::vector<std::string> resume_messages_;
};

TEST_F(BackgroundShellCompletionTest, ShellToolSchemaDocumentsOnCompletePolicy) {
    const auto definitions = registry_.definitions();
    const auto *shell = find_tool(definitions, "shell");
    ASSERT_NE(shell, nullptr);
    ASSERT_TRUE(shell->input_schema.contains("properties"));
    ASSERT_TRUE(shell->input_schema["properties"].contains("on_complete"));

    const auto &on_complete = shell->input_schema["properties"]["on_complete"];
    ASSERT_EQ(on_complete.at("type"), "object");
    ASSERT_TRUE(on_complete.contains("properties"));
    ASSERT_TRUE(on_complete["properties"].contains("mode"));
    ASSERT_TRUE(on_complete["properties"].contains("prompt"));
    EXPECT_EQ(on_complete["properties"]["mode"]["enum"], json::array({"inbox", "resume"}));
}

TEST_F(BackgroundShellCompletionTest, BackgroundShellDefaultsToInboxOnlyCompletionRouting) {
    const auto start_payload = start_background_shell({
        {"command", "printf 'done\\n'"},
    });

    const auto items = wait_for_inbox_size(1);
    ASSERT_EQ(items.size(), 1U);
    {
        std::scoped_lock lock(resume_messages_mutex_);
        EXPECT_TRUE(resume_messages_.empty());
    }

    const auto completion = json::parse(items.front().body);
    EXPECT_EQ(completion.at("type"), "background_process_completion");
    EXPECT_EQ(completion.at("process_id"), start_payload.at("process_id"));
    EXPECT_EQ(completion.at("on_complete").at("mode"), "inbox");
}

TEST_F(BackgroundShellCompletionTest, ResumeCompletionPreservesPromptInInboxAndResumePayload) {
    const std::string prompt = "Summarize the command result and continue the task.";
    const auto start_payload = start_background_shell({
        {"command", "printf 'ready\\n'"},
        {"on_complete", {{"mode", "resume"}, {"prompt", prompt}}},
    });

    const auto items = wait_for_inbox_size(1);
    ASSERT_EQ(items.size(), 1U);
    const auto resume_messages = wait_for_resume_messages_size(1);
    ASSERT_EQ(resume_messages.size(), 1U);

    const auto inbox_completion = json::parse(items.front().body);
    const auto resume_completion = json::parse(resume_messages.front());

    EXPECT_EQ(inbox_completion.at("process_id"), start_payload.at("process_id"));
    EXPECT_EQ(inbox_completion.at("on_complete").at("mode"), "resume");
    EXPECT_EQ(inbox_completion.at("on_complete").at("prompt"), prompt);
    EXPECT_EQ(resume_completion.at("process_id"), start_payload.at("process_id"));
    EXPECT_EQ(resume_completion.at("on_complete").at("mode"), "resume");
    EXPECT_EQ(resume_completion.at("on_complete").at("prompt"), prompt);
}

TEST_F(BackgroundShellCompletionTest, UnsupportedCompletionRoutingDoesNotAdvertiseOrAcceptOnComplete) {
    ToolRegistry unsupported_registry;
    register_builtin_tools(unsupported_registry, nullptr, workspace_root_.string());

    const auto definitions = unsupported_registry.definitions();
    const auto *shell = find_tool(definitions, "shell");
    ASSERT_NE(shell, nullptr);
    ASSERT_TRUE(shell->input_schema.contains("properties"));
    EXPECT_FALSE(shell->input_schema["properties"].contains("on_complete"));

    const auto result = unsupported_registry.execute(ToolUseBlock{
        .id = "unsupported-on-complete",
        .name = "shell",
        .input =
            {
                {"command", "printf 'ignored\\n'"},
                {"background", true},
                {"on_complete", {{"mode", "resume"}}},
            },
    });

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("on_complete"), std::string::npos);
    EXPECT_NE(result.content.find("not available"), std::string::npos);
}

TEST_F(BackgroundShellCompletionTest, CompletionPayloadSummariesAreScrubbedBeforePersistence) {
    const std::string secret = "sk-ABCDEFGHIJKLMNOPQRSTUVWX1234567890";
    BackgroundCompletionDispatcher dispatcher(&tool_context_);
    dispatcher.dispatch(BackgroundProcessCompletionEvent{
        .process_id = "proc-secret",
        .command = "printf 'secret'",
        .working_dir = workspace_root_.string(),
        .pid = 1234,
        .terminal_status = BackgroundProcessTerminalStatus::exited,
        .exit_code = 0,
        .stdout = {.tail = "stdout " + secret, .total_bytes = secret.size() + 7, .truncated = false},
        .stderr = {.tail = "stderr " + secret, .total_bytes = secret.size() + 7, .truncated = false},
        .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
    });

    const auto items = wait_for_inbox_size(1);
    ASSERT_EQ(items.size(), 1U);
    const auto resume_messages = wait_for_resume_messages_size(1);
    ASSERT_EQ(resume_messages.size(), 1U);

    const auto inbox_payload = json::parse(items.front().body);
    const auto resume_payload = json::parse(resume_messages.front());

    EXPECT_NE(inbox_payload.at("stdout").at("tail").get<std::string>().find("[REDACTED]"), std::string::npos);
    EXPECT_NE(inbox_payload.at("stderr").at("tail").get<std::string>().find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(inbox_payload.at("stdout").at("tail").get<std::string>().find(secret), std::string::npos);
    EXPECT_EQ(inbox_payload.at("stderr").at("tail").get<std::string>().find(secret), std::string::npos);
    EXPECT_NE(resume_payload.at("stdout").at("tail").get<std::string>().find("[REDACTED]"), std::string::npos);
    EXPECT_NE(resume_payload.at("stderr").at("tail").get<std::string>().find("[REDACTED]"), std::string::npos);
    EXPECT_EQ(resume_payload.at("stdout").at("tail").get<std::string>().find(secret), std::string::npos);
    EXPECT_EQ(resume_payload.at("stderr").at("tail").get<std::string>().find(secret), std::string::npos);
}

TEST_F(BackgroundShellCompletionTest, ResumeDispatchFailuresBecomeInboxVisibleNotes) {
    ToolRuntimeContext failing_context = tool_context_;
    failing_context.background_completion_runtime = make_background_completion_runtime_bindings(completion_owner_, runtime_.get(), [](const std::string &) {
        return std::optional<std::string>{"resume callback failed"};
    });

    BackgroundCompletionDispatcher dispatcher(&failing_context);
    dispatcher.dispatch(BackgroundProcessCompletionEvent{
        .process_id = "proc-test",
        .command = "printf 'done\\n'",
        .working_dir = workspace_root_.string(),
        .pid = 1234,
        .terminal_status = BackgroundProcessTerminalStatus::exited,
        .exit_code = 0,
        .stdout = {.tail = "done\\n", .total_bytes = 5, .truncated = false},
        .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
    });

    const auto items = wait_for_inbox_size(2);
    ASSERT_EQ(items.size(), 2U);

    const auto *completion_item = find_inbox_item_by_body_type(items, "background_process_completion");
    const auto *failure_item = find_inbox_item_by_body_type(items, "background_process_completion_resume_failure");
    ASSERT_NE(completion_item, nullptr);
    ASSERT_NE(failure_item, nullptr);

    const auto completion = json::parse(completion_item->body);
    const auto failure = json::parse(failure_item->body);

    EXPECT_EQ(completion.at("process_id"), "proc-test");
    EXPECT_EQ(completion.at("on_complete").at("mode"), "resume");
    EXPECT_EQ(failure.at("process_id"), "proc-test");
    EXPECT_EQ(failure.at("reason"), "resume callback failed");
}

TEST_F(BackgroundShellCompletionTest, DispatcherNoOpsAfterOwnerTokenExpiresEvenIfBindingsRemainAlive) {
    size_t resume_callback_count = 0;
    auto expiring_owner = std::make_shared<int>(1);
    auto expiring_state = make_background_completion_runtime_bindings(expiring_owner, runtime_.get(), [&resume_callback_count](const std::string &) {
        ++resume_callback_count;
        return std::optional<std::string>{};
    });
    ToolRuntimeContext expiring_context{
        .runtime_key = "runtime:test:expired",
        .agent_key = tool_context_.agent_key,
        .scope_key = "scope:test",
        .automation_runtime = runtime_.get(),
        .background_completion_runtime = expiring_state,
    };

    BackgroundCompletionDispatcher dispatcher(&expiring_context);
    expiring_owner.reset();

    const auto snapshot = expiring_state->snapshot();
    EXPECT_EQ(snapshot.owner_guard, nullptr);
    EXPECT_EQ(snapshot.automation_runtime, nullptr);
    EXPECT_FALSE(static_cast<bool>(snapshot.resume_callback));

    dispatcher.dispatch(BackgroundProcessCompletionEvent{
        .process_id = "proc-expired",
        .command = "printf 'expired'",
        .working_dir = workspace_root_.string(),
        .pid = 4321,
        .terminal_status = BackgroundProcessTerminalStatus::exited,
        .exit_code = 0,
        .stdout = {.tail = "done", .total_bytes = 4, .truncated = false},
        .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
    });

    EXPECT_TRUE(runtime_->list_inbox(tool_context_.agent_key).empty());
    EXPECT_EQ(resume_callback_count, 0U);
}

} // namespace
