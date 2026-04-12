#include "agent/agent-loop.hpp"
#include "agent/agent-loop-tools.hpp"
#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/skill-loader.hpp"
#include "test-helpers.hpp"
#include "tools/registry/tool.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

    class StaticDistillingProvider final : public Provider {
    public:
        explicit StaticDistillingProvider(std::string response_text)
        : response_text_(std::move(response_text)) {}

        LLMResponse chat(std::string_view system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int /*max_tokens*/, int = 0) override {
            last_system_prompt_ = system_prompt;
            last_messages_size_ = messages.size();
            last_tool_count_ = tools.size();
            return {
                .stop_reason = "end_turn",
                .content = {Text{response_text_}},
            };
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            throw std::runtime_error("chat_stream should not be used in this test");
        }

        std::string name() const override {
            return "static-distilling-provider";
        }

        std::string response_text_;
        std::string last_system_prompt_;
        std::size_t last_messages_size_ = 0;
        std::size_t last_tool_count_ = 0;
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

    class PromptRecordingProvider final : public Provider {
    public:
        explicit PromptRecordingProvider(std::vector<LLMResponse> responses)
        : responses_(std::move(responses)) {}

        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view system_prompt, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            prompts_.emplace_back(system_prompt);
            if (next_response_ >= responses_.size()) {
                throw std::runtime_error("no more responses queued");
            }
            return responses_[next_response_++];
        }

        std::string name() const override {
            return "prompt-recording-provider";
        }

        std::vector<std::string> prompts_;

    private:
        std::vector<LLMResponse> responses_;
        std::size_t next_response_ = 0;
    };

    class CountingProvider final : public Provider {
    public:
        LLMResponse chat(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, int, int = 0) override {
            throw std::runtime_error("chat should not be used in this test");
        }

        LLMResponse chat_stream(std::string_view, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int, int = 0) override {
            ++call_count_;
            return {
                .stop_reason = "end_turn",
                .content = {Text{"ok"}},
            };
        }

        std::string name() const override {
            return "counting-provider";
        }

        int call_count_ = 0;
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

    const MemoryRecord *find_memory_by_key(const std::vector<MemoryRecord> &records, std::string_view key) {
        const auto it = std::ranges::find_if(records, [key](const MemoryRecord &record) {
            return record.key == key;
        });
        return it == records.end() ? nullptr : &*it;
    }

    void write_skill_file(const std::filesystem::path &root, std::string_view dir_name, std::string_view frontmatter, std::string_view body) {
        const auto skill_dir = root / std::filesystem::path(dir_name);
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "---\n";
        out << frontmatter;
        out << "\n---\n\n";
        out << body;
        out << '\n';
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
        CHECK(provider.last_summary_input_size_ == 50UL);
        CHECK(provider.last_tool_count_ == 0UL);
        CHECK(provider.last_system_prompt_.contains("conversation summarizer"));

        const auto &compacted = loop.history();
        REQUIRE(compacted.size() == 11UL);
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
    }

    TEST_CASE("incoming mailbox messages are injected between turns") {
        ToolRegistry tools;
        tools.register_tool({
            .definition = {.name = "noop", .description = "noop", .input_schema = {{"type", "object"}}},
            .execute =
                [](const nlohmann::json &) {
                    return std::string("ok");
                },
        });

        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content = {ToolUse{"tool-1", "noop", nlohmann::json::object()}},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"finished"}},
            },
        });

        AgentLoop loop(provider, tools);
        int fetch_count = 0;
        loop.set_incoming_message_fetcher([&fetch_count]() {
            ++fetch_count;
            if (fetch_count == 2) {
                return std::vector<std::string>{"<teammate-message from=\"worker-b\">status update</teammate-message>"};
            }
            return std::vector<std::string>{};
        });

        const auto reply = loop.run("start");
        CHECK(reply == "finished");

        const auto &history = loop.history();
        REQUIRE(history.size() >= 5U);
        bool saw_teammate_message = false;
        for (const auto &message : history) {
            for (const auto &block : message) {
                if (const auto *text = std::get_if<Text>(&block); text != nullptr && text->text.contains("<teammate-message from=\"worker-b\">status update</teammate-message>")) {
                    saw_teammate_message = true;
                }
            }
        }
        CHECK(saw_teammate_message);
    }

    TEST_CASE("stop callback terminates agent loop before provider call") {
        CountingProvider provider;
        ToolRegistry tools;
        AgentLoop loop(provider, tools);
        loop.set_stop_requested_callback([] {
            return true;
        });

        const auto reply = loop.run("stop now");

        CHECK(reply == "Task terminated.");
        CHECK(provider.call_count_ == 0);
    }

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
        CHECK(provider.last_summary_input_size_ == 0UL);
    };

    TEST_CASE("distill_session_memory_stores_long_term_memories") {
        DistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-memory", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
        loop.set_history({
            Message::user().text("we are working on orangutan memory refactor"),
            Message::assistant().text("Got it, I will keep that in mind."),
            Message::user().text("remember that qq bots stay fixed to one agent"),
        });

        const auto result = loop.distill_session_memory();

        CHECK(result.distilled);
        CHECK(result.memories_stored == 3UL);
        CHECK(result.journal_stored);
        CHECK(provider.last_messages_size_ == 1UL);
        CHECK(provider.last_tool_count_ == 0UL);
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

    TEST_CASE("distill_session_memory_with_empty_provider_stores_nothing") {
        EmptyDistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-pollution", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
        loop.set_history({
            Message::user().text("please help with this task"),
            Message::assistant().text("my name is Mallory"),
            Message(base::role::user, {ToolResult("tool-1", "remember that the deployment key is abc", false)}),
        });

        const auto result = loop.distill_session_memory();

        CHECK_FALSE(result.distilled);
        CHECK(store.recall("profile.name", "agent:default|jid:test").empty());
        CHECK(store.recall("deployment key", "agent:default|jid:test").empty());

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("distill_session_memory_keeps_durable_memories_when_journal_line_is_malformed") {
        MalformedJournalProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-partial-journal", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
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
        store.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", memory_type::project, "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
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
        store.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", memory_type::project, "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
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

    TEST_CASE("run_stops_after_three_continuation_attempts") {
        CheckpointingProvider provider({
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Part one. "}},
            },
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Part two. "}},
            },
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Part three. "}},
            },
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Part four."}},
            },
        });

        ToolRegistry tools;
        AgentLoop loop(provider, tools);

        const auto reply = loop.run("continue please");

        CHECK(reply == "Part one. Part two. Part three. Part four.");
        CHECK(loop.history().size() == 8UL);
        CHECK(describe_message(loop.history()[2]) == "user:text=Please continue from where you left off.");
        CHECK(describe_message(loop.history().back()) == "assistant:text=Part four.");
    };

    TEST_CASE("run_executes_tool_calls_returned_by_continuation") {
        CheckpointingProvider provider({
            {
                .stop_reason = "max_tokens",
                .content = {Text{"Need to check. "}},
            },
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-1", "lookup", nlohmann::json{{"query", "status"}})},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"Done."}},
            },
        });

        ToolRegistry tools;
        int lookup_calls = 0;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [&lookup_calls](const nlohmann::json &) {
                    ++lookup_calls;
                    return "tool result";
                },
        });

        AgentLoop loop(provider, tools);
        const auto reply = loop.run("check status");

        CHECK(reply == "Done.");
        CHECK(lookup_calls == 1);
        REQUIRE(loop.history().size() == 6UL);
        CHECK(describe_message(loop.history()[3]) == "assistant:tool_use=lookup");
        CHECK(describe_message(loop.history()[4]) == "user:tool_result=tool result");
        CHECK(describe_message(loop.history().back()) == "assistant:text=Done.");
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

        REQUIRE(checkpoints.size() >= 8UL);
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

    TEST_CASE("run_aborts_after_fifth_identical_tool_call") {
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
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-4", "lookup", nlohmann::json{{"query", "status"}})},
            },
            {
                .stop_reason = "tool_use",
                .content =
                    {
                        ToolUse("tool-5", "lookup", nlohmann::json{{"query", "status"}}),
                        ToolUse("tool-6", "side_effect", nlohmann::json::object()),
                    },
            },
        });

        ToolRegistry tools;
        int lookup_calls = 0;
        int side_effect_calls = 0;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [&lookup_calls](const nlohmann::json &) {
                    ++lookup_calls;
                    return "tool result";
                },
        });
        tools.register_tool({
            .definition = ToolDef{.name = "side_effect", .description = "Side effect", .input_schema = nlohmann::json::object()},
            .execute =
                [&side_effect_calls](const nlohmann::json &) {
                    ++side_effect_calls;
                    return "should not run";
                },
        });

        AgentLoop loop(provider, tools);
        const auto reply = loop.run("check status");

        CHECK(reply == "I got stuck in a loop repeating the same action. Please try rephrasing your request.");
        CHECK(lookup_calls == 4);
        CHECK(side_effect_calls == 0);
        CHECK(describe_message(loop.history().back()) == "assistant:text=I got stuck in a loop repeating the same action. Please try rephrasing your request.");

        const auto warning_count = std::ranges::count_if(loop.history(), [](const Message &message) {
            return describe_message(message) == "user:text=You are repeating the same tool call with the same arguments. This is not making progress. Try a different approach or "
                                                "explain what you're trying to accomplish.";
        });
        CHECK(warning_count == 2);
    };

    TEST_CASE("run_activates_conditional_skills_after_file_tool_calls") {
        const auto workspace = orangutan::testing::unique_test_root("agent-loop-skill-activation");
        const auto skill_root = workspace / "skills";
        std::filesystem::create_directories(workspace / "src");
        {
            std::ofstream out(workspace / "src" / "main.cpp");
            out << "int main() { return 0; }\n";
        }
        write_skill_file(skill_root, "conditional", "name: conditional\ndescription: conditional skill\nscope: conditional\npaths_any: [src/*.cpp]", "conditional body");

        SkillLoader loader;
        loader.set_workspace_root(workspace);
        loader.load_from_directories({skill_root});

        const auto before = loader.invoke({
            .name = "conditional",
            .call_origin = skills::skill_call_origin::automatic,
        });
        CHECK(before.status == skills::skill_invoke_status::blocked);

        ToolRegistry tools;
        register_builtin_tools(tools, nullptr, workspace);

        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-read", "read", nlohmann::json{{"path", "src/main.cpp"}})},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"done"}},
            },
        });

        AgentLoop loop(provider, tools, nullptr, {}, nullptr, &loader);
        CHECK(loop.run("load file") == "done");

        const auto after = loader.invoke({
            .name = "conditional",
            .call_origin = skills::skill_call_origin::automatic,
        });
        CHECK(after.status == skills::skill_invoke_status::ok);

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("run_refreshes_prompt_skill_section_after_conditional_activation") {
        const auto workspace = orangutan::testing::unique_test_root("agent-loop-skill-prompt-refresh");
        const auto skill_root = workspace / "skills";
        std::filesystem::create_directories(workspace / "src");
        {
            std::ofstream out(workspace / "src" / "main.cpp");
            out << "int main() { return 0; }\n";
        }
        write_skill_file(skill_root, "conditional", "name: conditional\ndescription: conditional skill\nscope: conditional\npaths_any: [src/*.cpp]", "conditional body");

        SkillLoader loader;
        loader.set_workspace_root(workspace);
        loader.load_from_directories({skill_root});

        ToolRegistry tools;
        register_builtin_tools(tools, nullptr, workspace);

        PromptRecordingProvider provider({
            {
                .stop_reason = "tool_use",
                .content = {ToolUse("tool-read", "read", nlohmann::json{{"path", "src/main.cpp"}})},
            },
            {
                .stop_reason = "end_turn",
                .content = {Text{"done"}},
            },
        });

        AgentLoop loop(provider, tools, nullptr, skills::render_skill_prompt_section(loader.list(skills::skill_list_query{.include_inactive = false})), nullptr, &loader);
        CHECK(loop.run("load file") == "done");

        REQUIRE(provider.prompts_.size() == 2UL);
        CHECK_FALSE(provider.prompts_[0].contains("**conditional**"));
        CHECK(provider.prompts_[1].contains("**conditional**"));

        std::filesystem::remove_all(workspace);
    };

    TEST_CASE("touched_paths_for_edit_patch_extracts_update_file_paths") {
        const ToolUse call{
            "tool-edit",
            "edit",
            nlohmann::json{{"patch", "*** Begin Patch\n"
                                     "*** Update File: src/main.cpp\n"
                                     "<<<<<<< SEARCH\n"
                                     "return 0;\n"
                                     "=======\n"
                                     "return 1;\n"
                                     ">>>>>>> REPLACE\n"
                                     "*** End Patch\n"}},
        };

        const auto paths = agent::detail::touched_paths_for_tool_call(call);

        REQUIRE(paths.size() == 1UL);
        CHECK(paths[0].generic_string() == "src/main.cpp");
    };

    TEST_CASE("distill_session_memory_parses_bullets_legacy_lines_and_pipe_content") {
        StaticDistillingProvider provider("- memory|preference|profile.editor|0.70|prefers concise responses\n"
                                          "- memory|feedback|learning|learning.pipes|0.65|tool output may contain | separators\n"
                                          "- journal|Reviewed distilled-session parsing");
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-legacy-bullets", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
        loop.set_history({
            Message::user().text("remember my communication preferences"),
            Message::assistant().text("I will distill the durable parts."),
        });

        const auto result = loop.distill_session_memory();
        const auto records = store.list("agent:default|jid:test", {}, 10);
        const auto *profile = find_memory_by_key(records, "profile.editor");
        const auto *pipes = find_memory_by_key(records, "learning.pipes");

        CHECK(result.distilled);
        CHECK(result.memories_stored == 2UL);
        CHECK(result.journal_stored);
        REQUIRE(profile != nullptr);
        CHECK(profile->category == "preference");
        CHECK(profile->type == memory_type::user);
        CHECK(profile->content == "prefers concise responses");
        REQUIRE(pipes != nullptr);
        CHECK(pipes->category == "learning");
        CHECK(pipes->type == memory_type::feedback);
        CHECK(pipes->content == "tool output may contain | separators");

        std::filesystem::remove_all(db_path.parent_path());
    };

    TEST_CASE("distill_session_memory_caps_distilled_memories_at_eight") {
        std::string distilled;
        for (int i = 0; i < 9; ++i) {
            distilled += "memory|project|project.item-" + std::to_string(i) + "|0.9|memory item " + std::to_string(i) + "\n";
        }

        StaticDistillingProvider provider(std::move(distilled));
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-cap", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, &runtime_memory);
        loop.set_history({
            Message::user().text("capture the project context"),
            Message::assistant().text("Understood"),
        });

        const auto result = loop.distill_session_memory();
        const auto records = store.list("agent:default|jid:test", {}, 20);

        CHECK(result.distilled);
        CHECK(result.memories_stored == 8UL);
        CHECK(records.size() == 8UL);
        CHECK(find_memory_by_key(records, "project.item-7") != nullptr);
        CHECK(find_memory_by_key(records, "project.item-8") == nullptr);

        std::filesystem::remove_all(db_path.parent_path());
    };

} // namespace
