#include "features/agent/agent-loop.hpp"
#include "features/memory/memory.hpp"
#include "features/memory/runtime-memory.hpp"

#include <filesystem>
#include <gtest/gtest.h>

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

RuntimeMemory make_scoped_runtime_memory(MemoryStore &store, const std::string &scope) {
    return RuntimeMemory(store, RuntimeMemoryContext{.scope = scope});
}

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
            ADD_FAILURE() << "Unexpected content block in checkpoint";
            description += "unexpected";
            continue;
        }
        description += "tool_result=" + result->content;
    }

    return description;
}

std::vector<std::vector<std::string>> capture_checkpoint_descriptions(AgentLoop &loop, const std::string &user_input) {
    std::vector<std::vector<std::string>> checkpoints;
    (void)loop.run(user_input, {}, {}, [&checkpoints](const std::vector<Message> &history) {
        std::vector<std::string> snapshot;
        snapshot.reserve(history.size());
        for (const auto &message : history) {
            snapshot.push_back(describe_message(message));
        }
        checkpoints.push_back(std::move(snapshot));
    });
    return checkpoints;
}

} // namespace

TEST(AgentLoopTest, CompressHistorySummarizesOlderMessagesAndKeepsRecentTail) {
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

    EXPECT_TRUE(result.compacted);
    EXPECT_EQ(result.messages_before, 60);
    EXPECT_EQ(result.messages_after, 11);
    EXPECT_EQ(provider.last_summary_input_size_, 50U);
    EXPECT_EQ(provider.last_tool_count_, 0U);
    EXPECT_NE(provider.last_system_prompt_.find("conversation summarizer"), std::string::npos);

    const auto &compacted = loop.history();
    ASSERT_EQ(compacted.size(), 11);
    const auto *summary = std::get_if<TextBlock>(&compacted[0].content[0]);
    ASSERT_NE(summary, nullptr);
    EXPECT_NE(summary->text.find("Earlier conversation summary"), std::string::npos);

    for (size_t i = 0; i < 10; ++i) {
        const auto &expected = history[50 + i];
        const auto &actual = compacted[1 + i];
        EXPECT_EQ(actual.role, expected.role);
        ASSERT_EQ(actual.content.size(), expected.content.size());
        const auto *expected_text = std::get_if<TextBlock>(&expected.content[0]);
        const auto *actual_text = std::get_if<TextBlock>(&actual.content[0]);
        ASSERT_NE(expected_text, nullptr);
        ASSERT_NE(actual_text, nullptr);
        EXPECT_EQ(actual_text->text, expected_text->text);
    }
}

TEST(AgentLoopTest, CompressHistoryRequiresOlderMessagesBeyondRecentTail) {
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

    EXPECT_FALSE(result.compacted);
    EXPECT_EQ(result.messages_before, 10);
    EXPECT_EQ(result.messages_after, 10);
    EXPECT_EQ(result.status, "Not enough history to compress yet.");
    EXPECT_EQ(provider.last_summary_input_size_, 0U);
}

TEST(AgentLoopTest, DistillSessionMemoryStoresLongTermMemories) {
    DistillingProvider provider;
    ToolRegistry tools;

    const auto db_path = std::filesystem::temp_directory_path() / "orangutan_agent_loop_distill_memory_test.db";
    std::filesystem::remove(db_path);
    MemoryStore store(db_path.string());
    auto runtime_memory = make_scoped_runtime_memory(store, "agent:default|jid:test");

    AgentLoop loop(provider, tools, {}, &runtime_memory);
    loop.set_history({
        Message::user_text("we are working on orangutan memory refactor"),
        Message::assistant_text("Got it, I will keep that in mind."),
        Message::user_text("remember that qq bots stay fixed to one agent"),
    });

    const auto result = loop.distill_session_memory();

    EXPECT_TRUE(result.distilled);
    EXPECT_EQ(result.memories_stored, 3U);
    EXPECT_TRUE(result.journal_stored);
    EXPECT_EQ(provider.last_messages_size_, 1U);
    EXPECT_EQ(provider.last_tool_count_, 0U);
    EXPECT_NE(provider.last_system_prompt_.find("distilling long-term memory"), std::string::npos);

    const auto project = store.recall("project.current", "agent:default|jid:test");
    const auto decision = store.recall("decision.routing", "agent:default|jid:test");
    const auto learning = store.recall("learning.runtime-identity", "agent:default|jid:test");
    const auto journals = store.list("agent:default|jid:test", "journal", 10);

    EXPECT_NE(project.find("orangutan memory refactor"), std::string::npos);
    EXPECT_NE(decision.find("fixed to one agent"), std::string::npos);
    EXPECT_NE(learning.find("jid plus agent key"), std::string::npos);
    ASSERT_EQ(journals.size(), 1U);
    EXPECT_EQ(journals.front().source, "session:journal");
    EXPECT_NE(journals.front().content.find("markdown mirror behavior"), std::string::npos);

    std::filesystem::remove(db_path);
}

TEST(AgentLoopTest, DistillSessionMemoryAutoCaptureIgnoresAssistantAndToolResultText) {
    EmptyDistillingProvider provider;
    ToolRegistry tools;

    const auto db_path = std::filesystem::temp_directory_path() / "orangutan_agent_loop_distill_pollution_test.db";
    std::filesystem::remove(db_path);
    MemoryStore store(db_path.string());
    auto runtime_memory = make_scoped_runtime_memory(store, "agent:default|jid:test");

    AgentLoop loop(provider, tools, {}, &runtime_memory);
    loop.set_history({
        Message::user_text("please help with this task"),
        Message::assistant_text("my name is Mallory"),
        {.role = "user", .content = {ToolResultBlock{.tool_use_id = "tool-1", .content = "remember that the deployment key is abc", .is_error = false}}},
    });

    const auto result = loop.distill_session_memory();

    EXPECT_FALSE(result.distilled);
    EXPECT_EQ(store.recall("profile.name", "agent:default|jid:test"), "");
    EXPECT_EQ(store.recall("deployment key", "agent:default|jid:test"), "");

    std::filesystem::remove(db_path);
}

TEST(AgentLoopTest, DistillSessionMemoryKeepsDurableMemoriesWhenJournalLineIsMalformed) {
    MalformedJournalProvider provider;
    ToolRegistry tools;

    const auto db_path = std::filesystem::temp_directory_path() / "orangutan_agent_loop_distill_partial_journal_test.db";
    std::filesystem::remove(db_path);
    MemoryStore store(db_path.string());
    auto runtime_memory = make_scoped_runtime_memory(store, "agent:default|jid:test");

    AgentLoop loop(provider, tools, {}, &runtime_memory);
    loop.set_history({
        Message::user_text("we are working on orangutan memory refactor"),
        Message::assistant_text("Understood"),
    });

    const auto result = loop.distill_session_memory();

    EXPECT_TRUE(result.distilled);
    EXPECT_EQ(result.memories_stored, 1U);
    EXPECT_FALSE(result.journal_stored);
    EXPECT_NE(result.status.find("journaling was skipped"), std::string::npos);
    EXPECT_NE(store.recall("project.current", "agent:default|jid:test").find("orangutan memory refactor"), std::string::npos);
    EXPECT_TRUE(store.list("agent:default|jid:test", "journal", 10).empty());

    std::filesystem::remove(db_path);
}

TEST(AgentLoopTest, OrdinaryPromptsExcludeJournalEntriesFromRelevantMemories) {
    PromptCapturingProvider provider;
    ToolRegistry tools;

    const auto db_path = std::filesystem::temp_directory_path() / "orangutan_agent_loop_prompt_journal_exclusion_test.db";
    std::filesystem::remove(db_path);
    MemoryStore store(db_path.string());
    store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
    store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
    auto runtime_memory = make_scoped_runtime_memory(store, "agent:default|jid:test");

    AgentLoop loop(provider, tools, {}, &runtime_memory);
    (void)loop.run("what project am I working on?");

    EXPECT_NE(provider.last_system_prompt_.find("orangutan memory enhancements"), std::string::npos);
    EXPECT_EQ(provider.last_system_prompt_.find("Yesterday we debugged the failing mirror refresh."), std::string::npos);

    std::filesystem::remove(db_path);
}

TEST(AgentLoopTest, JournalQueriesCanIncludeJournalEntriesInRelevantMemories) {
    PromptCapturingProvider provider;
    ToolRegistry tools;

    const auto db_path = std::filesystem::temp_directory_path() / "orangutan_agent_loop_prompt_journal_inclusion_test.db";
    std::filesystem::remove(db_path);
    MemoryStore store(db_path.string());
    store.remember("project.current", "orangutan memory enhancements", "project", "agent:default|jid:test", "session:distilled", 0.9);
    store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", "agent:default|jid:test", "session:journal", 0.4);
    auto runtime_memory = make_scoped_runtime_memory(store, "agent:default|jid:test");

    AgentLoop loop(provider, tools, {}, &runtime_memory);
    (void)loop.run("what happened in the previous session journal?");

    EXPECT_NE(provider.last_system_prompt_.find("Yesterday we debugged the failing mirror refresh."), std::string::npos);

    std::filesystem::remove(db_path);
}

TEST(AgentLoopTest, RunCheckpointsEveryMeaningfulMutationInToolFlow) {
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

    EXPECT_EQ(checkpoints, (std::vector<std::vector<std::string>>{
                               {"user:text=check status"},
                               {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup"},
                               {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result"},
                               {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result", "assistant:text=All set."},
                           }));
}

TEST(AgentLoopTest, RunCheckpointsContinuationPromptBeforeContinuationCall) {
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

    EXPECT_EQ(checkpoints, (std::vector<std::vector<std::string>>{
                               {"user:text=continue please"},
                               {"user:text=continue please", "assistant:text=Part one. "},
                               {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off."},
                               {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off.", "assistant:text=Part two."},
                           }));
}

TEST(AgentLoopTest, RunCheckpointsLoopDetectionCorrectionBeforeRetry) {
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

    ASSERT_GE(checkpoints.size(), 8U);
    EXPECT_EQ(checkpoints[0], (std::vector<std::string>{"user:text=check status"}));
    EXPECT_EQ(checkpoints[1], (std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup"}));
    EXPECT_EQ(checkpoints[2], (std::vector<std::string>{"user:text=check status", "assistant:tool_use=lookup", "user:tool_result=tool result"}));
    EXPECT_EQ(checkpoints[3].back(), "assistant:tool_use=lookup");
    EXPECT_EQ(checkpoints[4].back(), "user:tool_result=tool result");
    EXPECT_EQ(checkpoints[5].back(), "assistant:tool_use=lookup");
    EXPECT_EQ(checkpoints[6].back(), "user:tool_result=tool result");
    EXPECT_EQ(checkpoints[7].back(), "user:text=You are repeating the same tool call with the same arguments. This is not making progress. Try a different approach or explain "
                                     "what you're trying to accomplish.");
}
