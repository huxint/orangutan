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

struct ResumeMessagesState {
    std::mutex mutex;
    std::vector<std::string> messages;
};

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

std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_test_background_completion_runtime_bindings(const std::shared_ptr<orangutan::automation::Store> &store,
                                                                                                            BackgroundCompletionResumeCallback resume_callback = {}) {
    return make_background_completion_runtime_bindings(
        [store](const orangutan::automation::InboxItem &item) {
            (void)store->insert_inbox(item);
        },
        std::move(resume_callback));
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

        store_ = std::make_shared<orangutan::automation::Store>(db_path.string());
        runtime_ = std::make_unique<orangutan::automation::Runtime>(*store_);
        resume_state_ = std::make_shared<ResumeMessagesState>();
        tool_context_ = ToolRuntimeContext{
            .runtime_key = "runtime:test:background-shell",
            .agent_key = "assistant",
            .scope_key = "scope:test",
            .automation_runtime = runtime_.get(),
        };
        tool_context_.background_completion_runtime = make_test_background_completion_runtime_bindings(store_, [resume_state = resume_state_](const std::string &message) {
            std::scoped_lock lock(resume_state->mutex);
            resume_state->messages.push_back(message);
            return std::optional<std::string>{};
        });

        register_builtin_tools(registry_, nullptr, workspace_root_.string(), &tool_context_);
    }

    void TearDown() override {
        runtime_.reset();
        store_.reset();
        resume_state_.reset();
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
                std::scoped_lock lock(resume_state_->mutex);
                if (resume_state_->messages.size() >= expected_size) {
                    return resume_state_->messages;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        ADD_FAILURE() << "background completion resume messages did not arrive in time";
        std::scoped_lock lock(resume_state_->mutex);
        return resume_state_->messages;
    }

    ToolRegistry registry_;
    ToolRuntimeContext tool_context_;
    std::unique_ptr<ScopedEnvVar> tmp_env_;
    std::filesystem::path workspace_root_;
    std::shared_ptr<orangutan::automation::Store> store_;
    std::unique_ptr<orangutan::automation::Runtime> runtime_;
    std::shared_ptr<ResumeMessagesState> resume_state_;
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

TEST_F(BackgroundShellCompletionTest, InboxOnlyCompletionRoutingExposesOnCompleteButRejectsResume) {
    ToolRuntimeContext inbox_only_context = tool_context_;
    inbox_only_context.background_completion_runtime = make_test_background_completion_runtime_bindings(store_);

    ToolRegistry inbox_only_registry;
    register_builtin_tools(inbox_only_registry, nullptr, workspace_root_.string(), &inbox_only_context);

    const auto definitions = inbox_only_registry.definitions();
    const auto *shell = find_tool(definitions, "shell");
    ASSERT_NE(shell, nullptr);
    ASSERT_TRUE(shell->input_schema.contains("properties"));
    ASSERT_TRUE(shell->input_schema["properties"].contains("on_complete"));
    EXPECT_EQ(shell->input_schema["properties"]["on_complete"]["properties"]["mode"]["enum"], json::array({"inbox"}));

    const auto result = inbox_only_registry.execute(ToolUseBlock{
        .id = "inbox-only-resume",
        .name = "shell",
        .input =
            {
                {"command", "printf 'ignored\\n'"},
                {"background", true},
                {"on_complete", {{"mode", "resume"}}},
            },
    });

    EXPECT_TRUE(result.is_error);
    EXPECT_NE(result.content.find("resume"), std::string::npos);
    EXPECT_NE(result.content.find("not available"), std::string::npos);
}

TEST_F(BackgroundShellCompletionTest, BackgroundShellDefaultsToInboxOnlyCompletionRouting) {
    const auto start_payload = start_background_shell({
        {"command", "printf 'done\\n'"},
    });

    const auto items = wait_for_inbox_size(1);
    ASSERT_EQ(items.size(), 1U);
    {
        std::scoped_lock lock(resume_state_->mutex);
        EXPECT_TRUE(resume_state_->messages.empty());
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

TEST_F(BackgroundShellCompletionTest, LargeResumePromptIsBoundedInPersistedAndResumePayload) {
    const std::string prompt(background_completion_prompt_max_chars * 4, 'p');
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
    const auto inbox_prompt = inbox_completion.at("on_complete").at("prompt").get<std::string>();
    const auto resume_prompt = resume_completion.at("on_complete").at("prompt").get<std::string>();

    EXPECT_EQ(inbox_completion.at("process_id"), start_payload.at("process_id"));
    EXPECT_EQ(inbox_prompt, resume_prompt);
    EXPECT_LT(inbox_prompt.size(), prompt.size());
    EXPECT_LE(inbox_prompt.size(), background_completion_prompt_max_chars);
    EXPECT_TRUE(inbox_prompt.ends_with("..."));
    EXPECT_LE(items.front().body.size(), background_completion_payload_max_bytes);
    EXPECT_LE(resume_messages.front().size(), background_completion_payload_max_bytes);
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
    failing_context.background_completion_runtime = make_test_background_completion_runtime_bindings(store_, [](const std::string &) {
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

} // namespace
