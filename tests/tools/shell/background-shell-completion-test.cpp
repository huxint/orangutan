#include "cli/single-shot.hpp"
#include "tools/registry/tool.hpp"
#include "automation/scheduler.hpp"
#include "automation/automation-store.hpp"
#include "tools/background/background-completion.hpp"
#include "utils/utf8.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

    struct ResumeMessagesState {
        std::mutex mutex;
        std::vector<std::string> messages;
    };

    class ScriptedProvider final : public Provider {
    public:
        using Step = std::function<LLMResponse(const std::vector<Message> &)>;

        explicit ScriptedProvider(std::vector<Step> steps)
        : steps_(std::move(steps)) {}

        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &messages, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            if (next_step_ >= steps_.size()) {
                throw std::runtime_error("no scripted response available");
            }
            return steps_[next_step_++](messages);
        }

        std::string name() const override {
            return "scripted-provider";
        }

    private:
        std::vector<Step> steps_;
        std::size_t next_step_ = 0;
    };

    const ToolDef *find_tool(const std::vector<ToolDef> &definitions, const std::string &name) {
        const auto it = std::ranges::find_if(definitions, [&](const ToolDef &definition) {
            return definition.name == name;
        });
        return it == definitions.end() ? nullptr : &(*it);
    }

    const orangutan::automation::InboxItem *find_inbox_item_by_body_type(const std::vector<orangutan::automation::InboxItem> &items, const std::string &type) {
        const auto it = std::ranges::find_if(items, [&](const orangutan::automation::InboxItem &item) {
            return nlohmann::json::parse(item.body).value("type", "") == type;
        });
        return it == items.end() ? nullptr : &(*it);
    }

    std::shared_ptr<const BackgroundCompletionRuntimeBindings> make_test_background_completion_runtime_bindings(const std::shared_ptr<orangutan::automation::Store> &store,
                                                                                                                BackgroundCompletionResumeCallback resume_callback = {}) {
        return make_background_completion_runtime_bindings(
            [store](const orangutan::automation::InboxItem &item) {
                static_cast<void>(store->insert_inbox(item));
            },
            std::move(resume_callback));
    }

    class BackgroundShellCompletionHarness {
    public:
        BackgroundShellCompletionHarness() {
            tmp_env_ = std::make_unique<ScopedEnvVar>("TMPDIR", orangutan::testing::test_tmp_root().string());
            workspace_root_ = orangutan::testing::test_tmp_root() / "background-shell-completion-workspace";
            const auto db_path = orangutan::testing::test_tmp_root() / "background-shell-completion.db";
            std::filesystem::remove_all(workspace_root_);
            std::filesystem::remove(db_path);
            std::filesystem::create_directories(workspace_root_);

            store_ = std::make_shared<orangutan::automation::Store>(db_path);
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

        ~BackgroundShellCompletionHarness() {
            runtime_.reset();
            store_.reset();
            resume_state_.reset();
            std::filesystem::remove_all(workspace_root_);
        }

        nlohmann::json start_background_shell(nlohmann::json input) {
            input["background"] = true;
            const auto result = registry_.execute(ToolUse("background-shell", "shell", std::move(input)));
            INFO(result.content);
            CHECK_FALSE(result.is_error);
            if (result.is_error) {
                return {};
            }

            return nlohmann::json::parse(result.content);
        }

        std::vector<orangutan::automation::InboxItem> wait_for_inbox_size(std::size_t expected_size, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                auto items = runtime_->list_inbox(tool_context_.agent_key);
                if (items.size() >= expected_size) {
                    return items;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }

            FAIL("background completion inbox items did not arrive in time");
            return runtime_->list_inbox(tool_context_.agent_key);
        }

        std::vector<std::string> wait_for_resume_messages_size(std::size_t expected_size, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
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

            FAIL("background completion resume messages did not arrive in time");
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

    TEST_CASE("sanitize_and_truncate_valid_prefix_preserves_codepoint_boundary_with_ellipsis") {
        CHECK(utf8::sanitize_and_truncate_valid_prefix("你好世界", 9, true) == std::string{"你好..."});
    };

    TEST_CASE("sanitize_and_truncate_valid_prefix_sanitizes_before_bounding") {
        const std::string malformed = std::string{"A"} + "\xFF" + "你好世界";
        CHECK(utf8::sanitize_and_truncate_valid_prefix(malformed, 8, false) == std::string{"A你好"});
        CHECK(utf8::sanitize_and_truncate_valid_prefix(malformed, 8, true) == std::string{"A你..."});
    };

    TEST_CASE("sanitize_and_truncate_valid_prefix_skips_ellipsis_when_not_truncated") {
        CHECK(utf8::sanitize_and_truncate_valid_prefix("ready", 16, true) == std::string{"ready"});
    };

    TEST_CASE("truncate_valid_suffix_preserves_codepoint_boundary") {
        CHECK(utf8::truncate_valid_suffix("ab你好", 4) == std::string{"好"});
    };

    TEST_CASE("shell_tool_schema_documents_on_complete_policy") {
        BackgroundShellCompletionHarness harness;

        const auto definitions = harness.registry_.definitions();
        const auto *shell = find_tool(definitions, "shell");
        INFO("shell tool definition missing");
        CHECK(shell != nullptr);
        if (shell == nullptr) {
            return;
        }

        CHECK(shell->input_schema.contains("properties"));
        CHECK(shell->input_schema["properties"].contains("on_complete"));

        const auto &on_complete = shell->input_schema["properties"]["on_complete"];
        CHECK(on_complete.at("type") == "object");
        CHECK(on_complete.contains("properties"));
        CHECK(on_complete["properties"].contains("mode"));
        CHECK(on_complete["properties"].contains("prompt"));
        CHECK(on_complete["properties"]["mode"]["enum"] == nlohmann::json::array({"inbox", "resume"}));
    };

    TEST_CASE("inbox_only_completion_routing_exposes_on_complete_but_rejects_resume") {
        BackgroundShellCompletionHarness harness;

        ToolRuntimeContext inbox_only_context = harness.tool_context_;
        inbox_only_context.background_completion_runtime = make_test_background_completion_runtime_bindings(harness.store_);

        ToolRegistry inbox_only_registry;
        register_builtin_tools(inbox_only_registry, nullptr, harness.workspace_root_.string(), &inbox_only_context);

        const auto definitions = inbox_only_registry.definitions();
        const auto *shell = find_tool(definitions, "shell");
        INFO("shell tool definition missing");
        CHECK(shell != nullptr);
        if (shell == nullptr) {
            return;
        }

        CHECK(shell->input_schema.contains("properties"));
        CHECK(shell->input_schema["properties"].contains("on_complete"));
        CHECK(shell->input_schema["properties"]["on_complete"]["properties"]["mode"]["enum"] == nlohmann::json::array({"inbox"}));

        const auto result = inbox_only_registry.execute(ToolUse("inbox-only-resume", "shell",
                                                                {
                                                                    {"command", "printf 'ignored\\n'"},
                                                                    {"background", true},
                                                                    {"on_complete", {{"mode", "resume"}}},
                                                                }));

        CHECK(result.is_error);
        CHECK(result.content.contains("resume"));
        CHECK(result.content.contains("not available"));
    };

    TEST_CASE("background_shell_defaults_to_inbox_only_completion_routing") {
        BackgroundShellCompletionHarness harness;

        const auto start_payload = harness.start_background_shell({
            {"command", "printf 'done\\n'"},
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        {
            std::scoped_lock lock(harness.resume_state_->mutex);
            CHECK(harness.resume_state_->messages.empty());
        }
        if (items.size() != 1U) {
            return;
        }

        const auto completion = nlohmann::json::parse(items.front().body);
        CHECK(completion.at("type") == "background_process_completion");
        CHECK(completion.at("process_id") == start_payload.at("process_id"));
        CHECK(completion.at("on_complete").at("mode") == "inbox");
    };

    TEST_CASE("resume_completion_preserves_prompt_in_inbox_and_resume_payload") {
        BackgroundShellCompletionHarness harness;

        const std::string prompt = "Summarize the command result and continue the task.";
        const auto start_payload = harness.start_background_shell({
            {"command", "printf 'ready\\n'"},
            {"on_complete", {{"mode", "resume"}, {"prompt", prompt}}},
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        const auto resume_messages = harness.wait_for_resume_messages_size(1);
        CHECK(resume_messages.size() == 1U);
        if (items.size() != 1U || resume_messages.size() != 1U) {
            return;
        }

        const auto inbox_completion = nlohmann::json::parse(items.front().body);
        const auto resume_completion = nlohmann::json::parse(resume_messages.front());

        CHECK(inbox_completion.at("process_id") == start_payload.at("process_id"));
        CHECK(inbox_completion.at("on_complete").at("mode") == "resume");
        CHECK(inbox_completion.at("on_complete").at("prompt") == prompt);
        CHECK(resume_completion.at("process_id") == start_payload.at("process_id"));
        CHECK(resume_completion.at("on_complete").at("mode") == "resume");
        CHECK(resume_completion.at("on_complete").at("prompt") == prompt);
    };

    TEST_CASE("large_resume_prompt_is_bounded_in_persisted_and_resume_payload") {
        BackgroundShellCompletionHarness harness;

        const std::string prompt_unit = "\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x9A\x80";
        std::string prompt;
        for (std::size_t i = 0; i < orangutan::tools::background_completion_prompt_max_chars; ++i) {
            prompt += prompt_unit;
        }
        const auto start_payload = harness.start_background_shell({
            {"command", "printf 'ready\\n'"},
            {"on_complete", {{"mode", "resume"}, {"prompt", prompt}}},
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        const auto resume_messages = harness.wait_for_resume_messages_size(1);
        CHECK(resume_messages.size() == 1U);
        if (items.size() != 1U || resume_messages.size() != 1U) {
            return;
        }

        const auto inbox_completion = nlohmann::json::parse(items.front().body);
        const auto resume_completion = nlohmann::json::parse(resume_messages.front());
        const auto inbox_prompt = inbox_completion.at("on_complete").at("prompt").get<std::string>();
        const auto resume_prompt = resume_completion.at("on_complete").at("prompt").get<std::string>();

        CHECK(inbox_completion.at("process_id") == start_payload.at("process_id"));
        CHECK(inbox_prompt == resume_prompt);
        CHECK(inbox_prompt.size() < prompt.size());
        CHECK(inbox_prompt.size() <= orangutan::tools::background_completion_prompt_max_chars);
        CHECK(inbox_prompt.ends_with("..."));
        CHECK(inbox_prompt.contains(prompt_unit));
        CHECK(items.front().body.size() <= orangutan::tools::background_completion_payload_max_bytes);
        CHECK(resume_messages.front().size() <= orangutan::tools::background_completion_payload_max_bytes);
    };

    TEST_CASE("unsupported_completion_routing_does_not_advertise_or_accept_on_complete") {
        BackgroundShellCompletionHarness harness;

        ToolRegistry unsupported_registry;
        register_builtin_tools(unsupported_registry, nullptr, harness.workspace_root_.string());

        const auto definitions = unsupported_registry.definitions();
        const auto *shell = find_tool(definitions, "shell");
        INFO("shell tool definition missing");
        CHECK(shell != nullptr);
        if (shell == nullptr) {
            return;
        }

        CHECK(shell->input_schema.contains("properties"));
        CHECK_FALSE(shell->input_schema["properties"].contains("on_complete"));

        const auto result = unsupported_registry.execute(ToolUse("unsupported-on-complete", "shell",
                                                                 {
                                                                     {"command", "printf 'ignored\\n'"},
                                                                     {"background", true},
                                                                     {"on_complete", {{"mode", "resume"}}},
                                                                 }));

        CHECK(result.is_error);
        CHECK(result.content.contains("on_complete"));
        CHECK(result.content.contains("not available"));
    };

    TEST_CASE("completion_payload_summaries_are_scrubbed_before_persistence") {
        BackgroundShellCompletionHarness harness;

        const std::string secret = "sk-ABCDEFGHIJKLMNOPQRSTUVWX1234567890";
        orangutan::tools::BackgroundCompletionDispatcher dispatcher(&harness.tool_context_);
        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-secret",
            .command = "printf 'secret'",
            .working_dir = harness.workspace_root_.string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "stdout " + secret, .total_bytes = secret.size() + 7, .truncated = false},
            .stderr = {.tail = "stderr " + secret, .total_bytes = secret.size() + 7, .truncated = false},
            .metadata = {{std::string(orangutan::tools::background_completion_mode_metadata_key), "resume"}},
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        const auto resume_messages = harness.wait_for_resume_messages_size(1);
        CHECK(resume_messages.size() == 1U);
        if (items.size() != 1U || resume_messages.size() != 1U) {
            return;
        }

        const auto inbox_payload = nlohmann::json::parse(items.front().body);
        const auto resume_payload = nlohmann::json::parse(resume_messages.front());

        CHECK(inbox_payload.at("stdout").at("tail").get<std::string>().contains("[REDACTED]"));
        CHECK(inbox_payload.at("stderr").at("tail").get<std::string>().contains("[REDACTED]"));
        CHECK_FALSE(inbox_payload.at("stdout").at("tail").get<std::string>().contains(secret));
        CHECK_FALSE(inbox_payload.at("stderr").at("tail").get<std::string>().contains(secret));
        CHECK(resume_payload.at("stdout").at("tail").get<std::string>().contains("[REDACTED]"));
        CHECK(resume_payload.at("stderr").at("tail").get<std::string>().contains("[REDACTED]"));
        CHECK_FALSE(resume_payload.at("stdout").at("tail").get<std::string>().contains(secret));
        CHECK_FALSE(resume_payload.at("stderr").at("tail").get<std::string>().contains(secret));
    };

    TEST_CASE("completion_titles_are_scrubbed_before_persistence") {
        BackgroundShellCompletionHarness harness;

        const std::string secret = "sk-ABCDEFGHIJKLMNOPQRSTUVWX1234567890";
        orangutan::tools::BackgroundCompletionDispatcher dispatcher(&harness.tool_context_);
        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-title",
            .command = "echo " + secret,
            .working_dir = harness.workspace_root_.string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(orangutan::tools::background_completion_mode_metadata_key), "inbox"}},
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        if (items.size() != 1U) {
            return;
        }

        CHECK_FALSE(items.front().title.contains(secret));
        CHECK(items.front().title.contains("[REDACTED]"));
    };

    TEST_CASE("invalid_utf8_output_and_prompt_still_produce_completion_payload") {
        BackgroundShellCompletionHarness harness;

        const std::string prompt = std::string("Prompt ") + "\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x9A\x80\xFF";
        const std::string stdout_tail = std::string("ok") + "\xFF\xFE\xE4\xB8\xAD";
        const std::string stderr_tail = std::string("err") + "\xFF";
        orangutan::tools::BackgroundCompletionDispatcher dispatcher(&harness.tool_context_);
        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-invalid-utf8",
            .command = std::string("printf '") + "\xE4\xBD\xA0" + "'",
            .working_dir = harness.workspace_root_.string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = stdout_tail, .total_bytes = stdout_tail.size(), .truncated = false},
            .stderr = {.tail = stderr_tail, .total_bytes = stderr_tail.size(), .truncated = false},
            .metadata =
                {
                    {std::string(orangutan::tools::background_completion_mode_metadata_key), "resume"},
                    {std::string(orangutan::tools::background_completion_prompt_metadata_key), prompt},
                },
        });

        const auto items = harness.wait_for_inbox_size(1);
        CHECK(items.size() == 1U);
        const auto resume_messages = harness.wait_for_resume_messages_size(1);
        CHECK(resume_messages.size() == 1U);
        if (items.size() != 1U || resume_messages.size() != 1U) {
            return;
        }

        const auto inbox_payload = nlohmann::json::parse(items.front().body);
        const auto resume_payload = nlohmann::json::parse(resume_messages.front());
        const std::string expected_prompt = "Prompt \xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x9A\x80";
        const std::string expected_stdout = "ok\xE4\xB8\xAD";
        const std::string expected_stderr = "err";

        CHECK(inbox_payload.at("on_complete").at("prompt") == expected_prompt);
        CHECK(resume_payload.at("on_complete").at("prompt") == expected_prompt);
        CHECK(inbox_payload.at("stdout").at("tail") == expected_stdout);
        CHECK(resume_payload.at("stdout").at("tail") == expected_stdout);
        CHECK(inbox_payload.at("stderr").at("tail") == expected_stderr);
        CHECK(resume_payload.at("stderr").at("tail") == expected_stderr);
    };

    TEST_CASE("completion_resume_runs_through_agent_execution_lease") {
        BackgroundShellCompletionHarness harness;

        std::promise<void> provider_started_promise;
        auto provider_started = provider_started_promise.get_future();
        ToolRegistry tools;
        ScriptedProvider provider({
            [&provider_started_promise](const std::vector<Message> &messages) {
                provider_started_promise.set_value();
                CHECK(not messages.empty());
                if (messages.empty()) {
                    return LLMResponse{};
                }

                CHECK(messages.back().role() == base::role::user);
                const auto *text = messages.back().begin() == messages.back().end() ? nullptr : std::get_if<Text>(&*messages.back().begin());
                INFO("expected trailing user text block");
                CHECK(text != nullptr);
                if (text != nullptr) {
                    CHECK(text->text.contains("\"type\": \"background_process_completion\""));
                }

                LLMResponse response;
                response.stop_reason = "end_turn";
                response.content.emplace_back(Text{"resume complete"});
                return response;
            },
        });
        AgentLoop agent(provider, tools);

        std::future<std::optional<std::string>> resume_result;
        {
            auto lease = harness.runtime_->acquire_agent_execution_lease(harness.tool_context_.agent_key);
            resume_result = std::async(std::launch::async, [&] {
                return orangutan::cli::run_completion_resume_message(agent,
                                                                     R"({
  "type": "background_process_completion",
  "process_id": "proc-lease"
})",
                                                                     harness.tool_context_.agent_key, harness.runtime_.get(), {}, true);
            });

            CHECK(provider_started.wait_for(std::chrono::milliseconds(150)) == std::future_status::timeout);
        }

        CHECK(provider_started.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        CHECK(resume_result.get() == std::nullopt);
        CHECK(agent.history().size() >= 2U);
        CHECK(agent.history().back().role() == base::role::assistant);
    };

    TEST_CASE("resume_dispatch_failures_become_inbox_visible_notes") {
        BackgroundShellCompletionHarness harness;

        const std::string secret = "sk-ABCDEFGHIJKLMNOPQRSTUVWX1234567890";
        ToolRuntimeContext failing_context = harness.tool_context_;
        failing_context.background_completion_runtime = make_test_background_completion_runtime_bindings(harness.store_, [](const std::string &) {
            return std::optional<std::string>{"resume callback failed"};
        });

        orangutan::tools::BackgroundCompletionDispatcher dispatcher(&failing_context);
        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-test",
            .command = "echo " + secret,
            .working_dir = harness.workspace_root_.string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(orangutan::tools::background_completion_mode_metadata_key), "resume"}},
        });

        const auto items = harness.wait_for_inbox_size(2);
        CHECK(items.size() == 2U);
        if (items.size() != 2U) {
            return;
        }

        const auto *completion_item = find_inbox_item_by_body_type(items, "background_process_completion");
        const auto *failure_item = find_inbox_item_by_body_type(items, "background_process_completion_resume_failure");
        INFO("completion inbox item missing");
        CHECK(completion_item != nullptr);
        INFO("failure inbox item missing");
        CHECK(failure_item != nullptr);
        if (completion_item == nullptr || failure_item == nullptr) {
            return;
        }

        const auto completion = nlohmann::json::parse(completion_item->body);
        const auto failure = nlohmann::json::parse(failure_item->body);

        CHECK(completion.at("process_id") == "proc-test");
        CHECK(completion.at("on_complete").at("mode") == "resume");
        CHECK(failure.at("process_id") == "proc-test");
        CHECK(failure.at("reason") == "resume callback failed");
        CHECK_FALSE(completion_item->title.contains(secret));
        CHECK_FALSE(failure_item->title.contains(secret));
        CHECK(completion_item->title.contains("[REDACTED]"));
        CHECK(failure_item->title.contains("[REDACTED]"));
    };

} // namespace
