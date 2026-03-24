#include "features/agent/agent-loop.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"
#include "test-helpers.hpp"

#include <filesystem>
#include "support/ut.hpp"
using namespace orangutan;

namespace {

class SummarizingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int /*max_tokens*/) override {
        last_system_prompt_ = system_prompt;
        last_summary_input_size_ = messages.size();
        last_tool_count_ = tools.size();
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "Earlier conversation summary"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used in this test");
    }

    std::string name() const override {
        return "summarizing-test-provider";
    }

    std::string last_system_prompt_;
    size_t last_summary_input_size_ = 0;
    size_t last_tool_count_ = 0;
};

class DistillingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &system_prompt, const std::vector<Message> &messages, const std::vector<ToolDef> &tools, int /*max_tokens*/) override {
        last_system_prompt_ = system_prompt;
        last_messages_size_ = messages.size();
        last_tool_count_ = tools.size();
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "memory|project|project.current|0.90|orangutan memory refactor\n"
                                          "memory|decision|decision.routing|0.80|qq bots stay fixed to one agent\n"
                                          "memory|learning|learning.runtime-identity|0.85|channel runtime identity should use jid plus agent key\n"
                                          "journal|Reviewed memory ranking and markdown mirror behavior"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used in this test");
    }

    std::string name() const override {
        return "distilling-test-provider";
    }

    std::string last_system_prompt_;
    size_t last_messages_size_ = 0;
    size_t last_tool_count_ = 0;
};

class EmptyDistillingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = ""}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used in this test");
    }

    std::string name() const override {
        return "empty-distilling-provider";
    }
};

class MalformedJournalProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "memory|project|project.current|0.90|orangutan memory refactor\n"
                                          "journal"}},
        };
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        throw std::runtime_error("chat_stream should not be used in this test");
    }

    std::string name() const override {
        return "malformed-journal-provider";
    }
};

class PromptCapturingProvider final : public Provider {
public:
    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(const std::string &system_prompt, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
        last_system_prompt_ = system_prompt;
        return {
            .stop_reason = "end_turn",
            .content = {TextBlock{.text = "ok"}},
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

    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, const StreamCallback &, int) override {
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
    size_t next_response_ = 0;
};

std::string describe_message(const Message &message) {
    std::string description = message.role + ":";
    bool first_block = true;
    for (const auto &block : message.content) {
        if (!first_block) {
            description += "|";
        }
        first_block = false;

        if (const auto *text = std::get_if<TextBlock>(&block)) {
            description += "text=" + text->text;
            continue;
        }
        if (const auto *tool = std::get_if<ToolUseBlock>(&block)) {
            description += "tool_use=" + tool->name;
            continue;
        }

        const auto *result = std::get_if<ToolResultBlock>(&block);
        if (result == nullptr) {
            boost::ut::expect(false) << "Unexpected content block in checkpoint";
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

boost::ut::suite agent_loop_suite = [] {
    using namespace boost::ut;

    "compress_history_summarizes_older_messages_and_keeps_recent_tail"_test = [] {
        SummarizingProvider provider;
        ToolRegistry tools;
        AgentLoop loop(provider, tools);

        std::vector<Message> history;
        history.reserve(60);
        for (int i = 0; i < 60; ++i) {
            history.push_back(i % 2 == 0 ? Message::user_text("user-" + std::to_string(i)) : Message::assistant_text("assistant-" + std::to_string(i)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        expect(result.compacted);
        expect(result.messages_before == 60_i);
        expect(result.messages_after == 11_i);
        expect(provider.last_summary_input_size_ == 50_ul);
        expect(provider.last_tool_count_ == 0_ul);
        expect(provider.last_system_prompt_.find("conversation summarizer") != std::string::npos);

        const auto &compacted = loop.history();
        expect((compacted.size() == 11_ul) >> fatal);
        const auto *summary = std::get_if<TextBlock>(&compacted[0].content[0]);
        expect((summary != nullptr) >> fatal);
        expect(summary->text.find("Earlier conversation summary") != std::string::npos);

        for (size_t i = 0; i < 10; ++i) {
            const auto &expected = history[50 + i];
            const auto &actual = compacted[1 + i];
            expect(actual.role == expected.role);
            expect((actual.content.size() == expected.content.size()) >> fatal);
            const auto *expected_text = std::get_if<TextBlock>(&expected.content[0]);
            const auto *actual_text = std::get_if<TextBlock>(&actual.content[0]);
            expect((expected_text != nullptr) >> fatal);
            expect((actual_text != nullptr) >> fatal);
            expect(actual_text->text == expected_text->text);
        }
    };

    "compress_history_requires_older_messages_beyond_recent_tail"_test = [] {
        SummarizingProvider provider;
        ToolRegistry tools;
        AgentLoop loop(provider, tools);

        std::vector<Message> history;
        history.reserve(10);
        for (int i = 0; i < 10; ++i) {
            history.push_back(Message::user_text("message-" + std::to_string(i)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        expect(not result.compacted);
        expect(result.messages_before == 10_i);
        expect(result.messages_after == 10_i);
        expect(result.status == "Not enough history to compress yet.");
        expect(provider.last_summary_input_size_ == 0_ul);
    };

    "distill_session_memory_stores_long_term_memories"_test = [] {
        DistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-memory", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user_text("we are working on orangutan memory refactor"),
            Message::assistant_text("Got it, I will keep that in mind."),
            Message::user_text("remember that qq bots stay fixed to one agent"),
        });

        const auto result = loop.distill_session_memory();

        expect(result.distilled);
        expect(result.memories_stored == 3_ul);
        expect(result.journal_stored);
        expect(provider.last_messages_size_ == 1_ul);
        expect(provider.last_tool_count_ == 0_ul);
        expect(provider.last_system_prompt_.find("distilling long-term memory") != std::string::npos);

        const auto project = store.recall("project.current", "agent:default|jid:test");
        const auto decision = store.recall("decision.routing", "agent:default|jid:test");
        const auto learning = store.recall("learning.runtime-identity", "agent:default|jid:test");
        const auto journals = store.list("agent:default|jid:test", "journal", 10);

        expect(project.find("orangutan memory refactor") != std::string::npos);
        expect(decision.find("fixed to one agent") != std::string::npos);
        expect(learning.find("jid plus agent key") != std::string::npos);
        expect((journals.size() == 1_ul) >> fatal);
        expect(journals.front().source == "session:journal");
        expect(journals.front().content.find("markdown mirror behavior") != std::string::npos);

        std::filesystem::remove_all(db_path.parent_path());
    };

    "distill_session_memory_auto_capture_ignores_assistant_and_tool_result_text"_test = [] {
        EmptyDistillingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-pollution", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user_text("please help with this task"),
            Message::assistant_text("my name is Mallory"),
            {.role = "user", .content = {ToolResultBlock{.tool_use_id = "tool-1", .content = "remember that the deployment key is abc", .is_error = false}}},
        });

        const auto result = loop.distill_session_memory();

        expect(not result.distilled);
        expect(store.recall("profile.name", "agent:default|jid:test") == "");
        expect(store.recall("deployment key", "agent:default|jid:test") == "");

        std::filesystem::remove_all(db_path.parent_path());
    };

    "distill_session_memory_keeps_durable_memories_when_journal_line_is_malformed"_test = [] {
        MalformedJournalProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-distill-partial-journal", "memory.db");
        MemoryStore store(db_path);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        loop.set_history({
            Message::user_text("we are working on orangutan memory refactor"),
            Message::assistant_text("Understood"),
        });

        const auto result = loop.distill_session_memory();

        expect(result.distilled);
        expect(result.memories_stored == 1_ul);
        expect(not result.journal_stored);
        expect(result.status.find("journaling was skipped") != std::string::npos);
        expect(store.recall("project.current", "agent:default|jid:test").find("orangutan memory refactor") != std::string::npos);
        expect(store.list("agent:default|jid:test", "journal", 10).empty());

        std::filesystem::remove_all(db_path.parent_path());
    };

    "ordinary_prompts_exclude_journal_entries_from_relevant_memories"_test = [] {
        PromptCapturingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-prompt-journal-exclusion", "memory.db");
        MemoryStore store(db_path);
        store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        static_cast<void>(loop.run("what project am I working on?"));

        expect(provider.last_system_prompt_.find("orangutan memory enhancements") != std::string::npos);
        expect(provider.last_system_prompt_.find("Yesterday we debugged the failing mirror refresh.") == std::string::npos);

        std::filesystem::remove_all(db_path.parent_path());
    };

    "journal_queries_can_include_journal_entries_in_relevant_memories"_test = [] {
        PromptCapturingProvider provider;
        ToolRegistry tools;

        const auto db_path = orangutan::testing::unique_test_db_path("agent-loop-prompt-journal-inclusion", "memory.db");
        MemoryStore store(db_path);
        store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
        auto runtime_memory = RuntimeMemory(store, RuntimeMemoryContext{.scope = "agent:default|jid:test"});

        AgentLoop loop(provider, tools, {}, &runtime_memory);
        static_cast<void>(loop.run("what happened in the previous session journal?"));

        expect(provider.last_system_prompt_.find("Yesterday we debugged the failing mirror refresh.") != std::string::npos);

        std::filesystem::remove_all(db_path.parent_path());
    };

    "run_checkpoints_every_meaningful_mutation_in_tool_flow"_test = [] {
        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content =
                    {
                        TextBlock{.text = "Looking that up."},
                        ToolUseBlock{.id = "tool-1", .name = "lookup", .input = json{{"query", "status"}}},
                    },
            },
            {
                .stop_reason = "end_turn",
                .content = {TextBlock{.text = "All set."}},
            },
        });

        ToolRegistry tools;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = json::object()},
            .execute =
                [](const json &) {
                    return "tool result";
                },
        });

        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "check status");

        expect(checkpoints == std::vector<std::vector<std::string>>{
                                  {"user:text=check status"},
                                  {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup"},
                                  {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result"},
                                  {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result", "assistant:text=All set."},
                              });
    };

    "run_checkpoints_continuation_prompt_before_continuation_call"_test = [] {
        CheckpointingProvider provider({
            {
                .stop_reason = "max_tokens",
                .content = {TextBlock{.text = "Part one. "}},
            },
            {
                .stop_reason = "end_turn",
                .content = {TextBlock{.text = "Part two."}},
            },
        });

        ToolRegistry tools;
        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "continue please");

        expect(checkpoints == std::vector<std::vector<std::string>>{
                                  {"user:text=continue please"},
                                  {"user:text=continue please", "assistant:text=Part one. "},
                                  {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off."},
                                  {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off.", "assistant:text=Part two."},
                              });
    };

    "run_checkpoints_loop_detection_correction_before_retry"_test = [] {
        CheckpointingProvider provider({
            {
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "tool-1", .name = "lookup", .input = json{{"query", "status"}}}},
            },
            {
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "tool-2", .name = "lookup", .input = json{{"query", "status"}}}},
            },
            {
                .stop_reason = "tool_use",
                .content = {ToolUseBlock{.id = "tool-3", .name = "lookup", .input = json{{"query", "status"}}}},
            },
            {
                .stop_reason = "end_turn",
                .content = {TextBlock{.text = "Stopping loop."}},
            },
        });

        ToolRegistry tools;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = json::object()},
            .execute =
                [](const json &) {
                    return "tool result";
                },
        });

        AgentLoop loop(provider, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "check status");

        expect((checkpoints.size() >= 8_ul) >> fatal);
        expect(checkpoints[0] == std::vector<std::string>{"user:text=check status"});
        expect(checkpoints[1] == std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup"});
        expect(checkpoints[2] == std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup", "user:tool_result=tool result"});
        expect(checkpoints[3].back() == "assistant:tool_use=lookup");
        expect(checkpoints[4].back() == "user:tool_result=tool result");
        expect(checkpoints[5].back() == "assistant:tool_use=lookup");
        expect(checkpoints[6].back() == "user:tool_result=tool result");
        expect(checkpoints[7].back() == "user:text=You are repeating the same tool call with the same arguments. This is not making progress. Try a different approach or explain "
                                        "what you're trying to accomplish.");
    };
};

} // namespace
