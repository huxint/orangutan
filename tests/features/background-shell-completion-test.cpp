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
        tool_context_ = ToolRuntimeContext{
            .runtime_key = "runtime:test:background-shell",
            .agent_key = "assistant",
            .scope_key = "scope:test",
            .automation_runtime = runtime_.get(),
            .background_completion_resume =
                [this](const std::string &message) {
                    std::scoped_lock lock(resume_messages_mutex_);
                    resume_messages_.push_back(message);
                    return std::optional<std::string>{};
                },
        };

        register_builtin_tools(registry_, nullptr, workspace_root_.string(), &tool_context_);
    }

    void TearDown() override {
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

TEST_F(BackgroundShellCompletionTest, ResumeDispatchFailuresBecomeInboxVisibleNotes) {
    ToolRuntimeContext failing_context = tool_context_;
    failing_context.background_completion_resume = [](const std::string &) {
        return std::optional<std::string>{"resume callback failed"};
    };

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

} // namespace
