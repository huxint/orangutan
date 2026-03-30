#include "features/agent/agent-loop.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include <catch2/catch_test_macros.hpp>
using namespace orangutan;

namespace {

    class SummarizingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int /*max_tokens*/, int = 0) override {
            last_system_prompt_ = system_prompt;
            last_summary_input_size_ = messages.size();
            last_tool_count_ = tools.size();
            return {
                .stop_reason = "end_turn",
                .content = {Text{"Earlier conversation summary"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            throw std::runtime_error("chat_stream should not be used in this test");
        }

        std::string name() const override {
            return "summarizing-test-provider";
        }

        std::string last_system_prompt_;
        std::size_t last_summary_input_size_ = 0;
        std::size_t last_tool_count_ = 0;
    };

    class DistillingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int /*max_tokens*/, int = 0) override {
            last_system_prompt_ = system_prompt;
            last_messages_size_ = messages.size();
            last_tool_count_ = tools.size();
            return {
                .stop_reason = "end_turn",
                .content = {Text{"memory|project|project.current|0.90|orangutan memory refactor\n"
                                 "memory|decision|decision.routing|0.80|qq bots stay fixed to one agent\n"
                                 "memory|learning|learning.runtime-identity|0.85|channel runtime identity should use jid plus agent key\n"
                                 "journal|Reviewed memory ranking and markdown mirror behavior"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            throw std::runtime_error("chat_stream should not be used in this test");
        }

        std::string name() const override {
            return "distilling-test-provider";
        }

        std::string last_system_prompt_;
        std::size_t last_messages_size_ = 0;
        std::size_t last_tool_count_ = 0;
    };

    class EmptyDistillingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            return {
                .stop_reason = "end_turn",
                .content = {Text{""}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            throw std::runtime_error("chat_stream should not be used in this test");
        }

        std::string name() const override {
            return "empty-distilling-provider";
        }
    };

    class MalformedJournalProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            return {
                .stop_reason = "end_turn",
                .content = {Text{"memory|project|project.current|0.90|orangutan memory refactor\n"
                                 "journal"}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            throw std::runtime_error("chat_stream should not be used in this test");
        }

        std::string name() const override {
            return "malformed-journal-provider";
        }
    };

    class PromptCapturingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            last_system_prompt_ = system_prompt;
            return {
                .stop_reason = "end_turn",
                .content = {Text{"ok"}},
            };
        }

        std::string name() const override {
            return "prompt-capturing-provider";
        }

        std::string last_system_prompt_;
    };

    class CheckpointingProvider final : public Provider {
    public:
        explicit CheckpointingProvider(std::vector<LLMResponse> responses)
        : responses_(std::move(responses)) {}

        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            if (next_response_ >= responses_.size()) {
                throw std::runtime_error("no more responses queued");
            }
            return responses_[next_response_++];
        }

        std::string name() const override {
            return "checkpointing-provider";
        }

    private:
        std::vector<LLMResponse> responses_;
        std::size_t next_response_ = 0;
    };

    std::string describe_message(const Message &message) {
        std::string description = std::string(magic_enum::enum_name(message.role())) + ":";
        bool first_block = true;
        for (const auto &block : message) {
            if (!first_block) {
                description += "|";
            }
            first_block = false;

            if (const auto *text = std::get_if<Text>(&block)) {
                description += "text=" + text->text;
                continue;
            }
            if (const auto *tool = std::get_if<ToolUse>(&block)) {
                description += "tool_use=" + tool->name;
                continue;
            }

            const auto *result = std::get_if<ToolResult>(&block);
            if (result == nullptr) {
                FAIL("Unexpected content block in checkpoint");
                description += "unexpected";
                continue;
            }
            description += "tool_result=" + result->content;
        }

        return description;
    }

    std::vector<std::vector<std::string>> capture_checkpoint_descriptions(AgentLoop &loop, const std::string &user_input) {
        std::vector<std::vector<std::string>> checkpoints;
        static_cast<void>(loop.run(user_input, {}, {}, [&checkpoints](const std::vector<Message> &history) {
            std::vector<std::string> snapshot;
            snapshot.reserve(history.size());
            for (const auto &message : history) {
                snapshot.push_back(describe_message(message));
            }
            checkpoints.push_back(std::move(snapshot));
        }));
        return checkpoints;
    }

    TEST_CASE("compress_history_summarizes_older_messages_and_keeps_recent_tail") {
        SummarizingProvider provider;
        ToolRegistry tools;
        AgentLoop loop(provider, tools);

        std::vector<Message> history;
        history.reserve(60);
        for (int i = 0; i < 60; ++i) {
            history.push_back(i % 2 == 0 ? Message::user().text("user-" + std::to_string(i)) : Message::assistant().text("assistant-" + std::to_string(i)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        CHECK(result.compacted);
        CHECK(result.messages_before == 60);
        CHECK(result.messages_after == 11);
        CHECK(provider.last_summary_input_size_ == 50ul);
        CHECK(provider.last_tool_count_ == 0ul);
        CHECK(provider.last_system_prompt_.contains("conversation summarizer"));

        const auto &compacted = loop.history();
        REQUIRE(compacted.size() == 11ul);
        const auto *summary = std::get_if<Text>(&*compacted[0].begin());
        REQUIRE(summary != nullptr);
        CHECK(summary->text.contains("Earlier conversation summary"));

        for (std::size_t i = 0; i < 10; ++i) {
            const auto &expected = history[50 + i];
            const auto &actual = compacted[1 + i];
            CHECK(actual.role() == expected.role());
            REQUIRE(std::distance(actual.begin(), actual.end()) == std::distance(expected.begin(), expected.end()));
            const auto *expected_text = std::get_if<Text>(&*expected.begin());
            const auto *actual_text = std::get_if<Text>(&*actual.begin());
            REQUIRE(expected_text != nullptr);
            REQUIRE(actual_text != nullptr);
            CHECK(actual_text->text == expected_text->text);
        }
    };

    TEST_CASE("compress_history_requires_older_messages_beyond_recent_tail") {
        SummarizingProvider provider;
        ToolRegistry tools;
        AgentLoop loop(provider, tools);

        std::vector<Message> history;
        history.reserve(10);
        for (int i = 0; i < 10; ++i) {
            history.push_back(Message::user().text("message-" + std::to_string(i)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        CHECK_FALSE(result.compacted);
        CHECK(result.messages_before == 10);
        CHECK(result.messages_after == 10);
        CHECK(result.status == "Not enough history to compress yet.");
        CHECK(provider.last_summary_input_size_ == 0ul);
    };

    TEST_CASE("distill_session_memory_stores_long_term_memories") {
        DistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-memory", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user().text("we are working on orangutan memory refactor"),
            Message::assistant().text("Got it, I will keep that in mind."),
            Message::user().text("remember that qq bots stay fixed to one agent"),
        });

        const auto result = loop.distill_session_memory();

        CHECK(result.distilled);
        CHECK(result.memories_stored == 3ul);
        CHECK(result.journal_stored);
        CHECK(provider.last_messages_size_ == 1UL);
        CHECK(provider.last_tool_count_ == 0ul);
        CHECK(provider.last_system_prompt_.contains("distilling long-term memory"));

        const auto project = store.recall("project.current", "agent:default|jid:test");
        const auto decision = store.recall("decision.routing", "agent:default|jid:test");
        const auto learning = store.recall("learning.runtime-identity", "agent:default|jid:test");
        const auto journals = store.list("agent:default|jid:test", "journal", 10);

        CHECK(project.contains("orangutan memory refactor"));
        CHECK(decision.contains("fixed to one agent"));
        CHECK(learning.contains("jid plus agent key"));
        REQUIRE(journals.size() == 1UL);
        CHECK(journals.front().source == "session:journal");
        CHECK(journals.front().content.contains("markdown mirror behavior"));

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("distill_session_memory_auto_capture_ignores_assistant_and_tool_result_text") {
        EmptyDistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-pollution", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user().text("please help with this task"),
            Message::assistant().text("my name is Mallory"),
            Message(base::role::user, {ToolResult("tool-1", "remember that the deployment key is abc", false)}),
        });

        const auto result = loop.distill_session_memory();

        CHECK_FALSE(result.distilled);
        CHECK(store.recall("profile.name", "agent:default|jid:test") == "");
        CHECK(store.recall("deployment key", "agent:default|jid:test") == "");

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("distill_session_memory_keeps_durable_memories_when_journal_line_is_malformed") {
        MalformedJournalProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-partial-journal", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user().text("we are working on orangutan memory refactor"),
            Message::assistant().text("Understood"),
        });

        const auto result = loop.distill_session_memory();

        CHECK(result.distilled);
        CHECK(result.memories_stored == 1UL);
        CHECK_FALSE(result.journal_stored);
        CHECK(result.status.contains("journaling was skipped"));
        CHECK(store.recall("project.current", "agent:default|jid:test").contains("orangutan memory refactor"));
        CHECK(store.list("agent:default|jid:test", "journal", 10).empty());

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("ordinary_prompts_exclude_journal_entries_from_relevant_memories") {
        PromptCapturingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-prompt-journal-exclusion", "memory.db");
        MemoryStore store(db_path);
        store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        static_cast<void>(loop.run("what project am I working on?"));

        CHECK(provider.last_system_prompt_.contains("orangutan memory enhancements"));
        CHECK_FALSE(provider.last_system_prompt_.contains("Yesterday we debugged the failing mirror refresh."));

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("journal_queries_can_include_journal_entries_in_relevant_memories") {
        PromptCapturingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-prompt-journal-inclusion", "memory.db");
        MemoryStore store(db_path);
        store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        static_cast<void>(loop.run("what happened in the previous session journal?"));

        CHECK(provider.last_system_prompt_.contains("Yesterday we debugged the failing mirror refresh."));

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("run_checkpoints_every_meaningful_mutation_in_tool_flow") {
        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content =
                    {
                        Text{"Looking that up."},
                        ToolUse("tool-1", "lookup", nlohmann::json{{"query", "status"}}),
                    },
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"All set."}},
            },
        });

        ToolRegistry tools;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return "tool result";
                },
        });

        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "check status");

        CHECK(checkpoints == std::vector<std::vector<std::string>>{
                                 {"user:text=check status"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result", "assistant:text=All set."},
                             });
    };

    TEST_CASE("run_checkpoints_continuation_prompt_before_continuation_call") {
        CheckpointingProvider provider({
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Part one. "}},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"Part two."}},
            },
        });

        ToolRegistry tools;
        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "continue please");

        CHECK(checkpoints == std::vector<std::vector<std::string>>{
                                 {"user:text=continue please"},
                                 {"user:text=continue please", "assistant:text=Part one. "},
                                 {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off."},
                                 {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off.", "assistant:text=Part two."},
                             });
    };

    TEST_CASE("run_checkpoints_loop_detection_correction_before_retry") {
        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-1", "lookup", nlohmann::json{{"query", "status"}})},
            },
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-2", "lookup", nlohmann::json{{"query", "status"}})},
            },
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-3", "lookup", nlohmann::json{{"query", "status"}})},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"Stopping loop."}},
            },
        });

        ToolRegistry tools;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return "tool result";
                },
        });

        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "check status");

        REQUIRE(checkpoints.size() >= 8ul);
        CHECK(checkpoints[0] == std::vector<std::string>{"user:text=check status"});
        CHECK(checkpoints[1] == std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup"});
        CHECK(checkpoints[2] == std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup", "user:tool_result=tool result"});
        CHECK(checkpoints[3].back() == "assistant:tool_use=lookup");
        CHECK(checkpoints[4].back() == "user:tool_result=tool result");
        CHECK(checkpoints[5].back() == "assistant:tool_use=lookup");
        CHECK(checkpoints[6].back() == "user:tool_result=tool result");
        CHECK(checkpoints[7].back() == "user:text=You are repeating the same tool call with the same arguments. This is not making progress. Try a different approach or explain "
                                       "what you're trying to accomplish.");
    };

} // namespace
