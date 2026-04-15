#include "agent/agent-loop.hpp"
#include "agent/agent-loop-tools.hpp"
#include "memory/memory-store.hpp"
#include "memory/runtime-memory.hpp"
#include "skills/runtime.hpp"
#include "skills/skill-loader.hpp"
#include "test-helpers.hpp"
#include "test-provider-support.hpp"
#include "tools/registry/tool.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <magic_enum/magic_enum.hpp>

using namespace orangutan;

namespace {

    [[nodiscard]]
    LLMResponse make_response(response_stop_reason stop_reason, std::vector<Content> content) {
        return LLMResponse{
            .stop_reason = stop_reason,
            .content = std::move(content),
        };
    }

    class ScriptedProvider {
    public:
        using send_hook = std::function<void(const providers::ProviderRequest &, const providers::ProviderEventSink &, std::size_t)>;

        explicit ScriptedProvider(std::vector<LLMResponse> responses, send_hook hook = {}, std::string model = "test-model")
        : backend_(testing::make_fake_provider_backend([this](const providers::ProviderRoute &route, const providers::ProviderRequest &request,
                                                              const providers::ProviderEventSink &sink) {
              prompts_.push_back(request.system_prompt);
              message_counts_.push_back(request.messages.size());
              tool_counts_.push_back(request.tools.size());

              if (hook_ != nullptr) {
                  hook_(request, sink, next_response_);
              }

              if (next_response_ >= responses_.size()) {
                  throw std::runtime_error("no more scripted responses");
              }

              return providers::ProviderResult{
                  .response = responses_[next_response_++],
                  .usage_snapshot = {},
                  .active_target = route.primary,
              };
          })),
          responses_(std::move(responses)),
          hook_(std::move(hook)),
          system(backend_),
          route(testing::make_test_route(std::move(model))) {
            backend_->set_label("scripted-provider");
        }

        [[nodiscard]]
        std::size_t invocation_count() const {
            return prompts_.size();
        }

        [[nodiscard]]
        const std::vector<std::string> &prompts() const {
            return prompts_;
        }

        [[nodiscard]]
        const std::vector<std::size_t> &message_counts() const {
            return message_counts_;
        }

        [[nodiscard]]
        const std::vector<std::size_t> &tool_counts() const {
            return tool_counts_;
        }

        std::shared_ptr<testing::FakeProviderBackend> backend_;
        std::vector<LLMResponse> responses_;
        send_hook hook_;
        std::size_t next_response_ = 0;
        std::vector<std::string> prompts_;
        std::vector<std::size_t> message_counts_;
        std::vector<std::size_t> tool_counts_;
        providers::ProviderSystem system;
        providers::ProviderRoute route;
    };

    [[nodiscard]]
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
            REQUIRE(result != nullptr);
            description += "tool_result=" + result->content;
        }

        return description;
    }

    [[nodiscard]]
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

    [[nodiscard]]
    const MemoryRecord *find_memory_by_key(const std::vector<MemoryRecord> &records, std::string_view key) {
        const auto it = std::ranges::find_if(records, [key](const MemoryRecord &record) {
            return record.key == key;
        });
        return it == records.end() ? nullptr : &(*it);
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
        ScriptedProvider provider({testing::make_text_response("Earlier conversation summary")});
        ToolRegistry tools;
        AgentLoop loop(provider.system, provider.route, tools);

        std::vector<Message> history;
        history.reserve(60);
        for (int index = 0; index < 60; ++index) {
            history.push_back(index % 2 == 0 ? Message::user().text("user-" + std::to_string(index))
                                             : Message::assistant().text("assistant-" + std::to_string(index)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        CHECK(result.compacted);
        CHECK(result.messages_before == 60);
        CHECK(result.messages_after == 11);
        REQUIRE(provider.message_counts().size() == 1UL);
        CHECK(provider.message_counts().front() == 50UL);
        CHECK(provider.tool_counts().front() == 0UL);
        CHECK(provider.prompts().front().contains("conversation summarizer"));

        const auto &compacted = loop.history();
        REQUIRE(compacted.size() == 11UL);
        const auto *summary = std::get_if<Text>(&*compacted.front().begin());
        REQUIRE(summary != nullptr);
        CHECK(summary->text.contains("Earlier conversation summary"));
    }

    TEST_CASE("compress_history_requires_older_messages_beyond_recent_tail") {
        ScriptedProvider provider({testing::make_text_response("unused")});
        ToolRegistry tools;
        AgentLoop loop(provider.system, provider.route, tools);

        std::vector<Message> history;
        history.reserve(10);
        for (int index = 0; index < 10; ++index) {
            history.push_back(Message::user().text("message-" + std::to_string(index)));
        }
        loop.set_history(history);

        const auto result = loop.compress_history();

        CHECK_FALSE(result.compacted);
        CHECK(result.messages_before == 10);
        CHECK(result.messages_after == 10);
        CHECK(result.status == "Not enough history to compress yet.");
        CHECK(provider.invocation_count() == 0UL);
    }

    TEST_CASE("incoming mailbox messages are injected between turns") {
        ToolRegistry tools;
        tools.register_tool({
            .definition = {.name = "noop", .description = "noop", .input_schema = {{"type", "object"}}},
            .execute =
                [](const nlohmann::json &) {
                    return std::string{"ok"};
                },
        });

        ScriptedProvider provider({
            make_response(response_stop_reason::tool_use, {ToolUse{"tool-1", "noop", nlohmann::json::object()}}),
            testing::make_text_response("finished"),
        });
        AgentLoop loop(provider.system, provider.route, tools);

        int fetch_count = 0;
        loop.set_incoming_message_fetcher([&fetch_count] {
            ++fetch_count;
            if (fetch_count == 2) {
                return std::vector<std::string>{"<teammate-message from=\"worker-b\">status update</teammate-message>"};
            }
            return std::vector<std::string>{};
        });

        const auto reply = loop.run("start");

        CHECK(reply == "finished");
        const auto &history = loop.history();
        REQUIRE(history.size() >= 5UL);
        CHECK(std::ranges::any_of(history, [](const Message &message) {
            return std::ranges::any_of(message, [](const Content &block) {
                const auto *text = std::get_if<Text>(&block);
                return text != nullptr && text->text.contains("<teammate-message from=\"worker-b\">status update</teammate-message>");
            });
        }));
    }

    TEST_CASE("stop callback terminates_agent_loop_before_provider_call") {
        ScriptedProvider provider({testing::make_text_response("unused")});
        ToolRegistry tools;
        AgentLoop loop(provider.system, provider.route, tools);
        loop.set_stop_requested_callback([] {
            return true;
        });

        const auto reply = loop.run("stop now");

        CHECK(reply == "Task terminated.");
        CHECK(provider.invocation_count() == 0UL);
    }

    TEST_CASE("distill_session_memory_stores_long_term_memories") {
        ScriptedProvider provider({testing::make_text_response("memory|project|project.current|0.90|orangutan memory refactor\n"
                                                               "memory|decision|decision.routing|0.80|qq bots stay fixed to one agent\n"
                                                               "memory|learning|learning.runtime-identity|0.85|channel runtime identity should use jid plus agent key\n"
                                                               "journal|Reviewed memory ranking and markdown mirror behavior")});
        ToolRegistry tools;

        const auto db_path = testing::unique_test_db_path("agent-loop-distill-memory", "memory.db");
        MemoryStore store(db_path);
        RuntimeMemory runtime_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});
        AgentLoop loop(provider.system, provider.route, tools, &runtime_memory);
        loop.set_history({
            Message::user().text("we are working on orangutan memory refactor"),
            Message::assistant().text("Got it, I will keep that in mind."),
            Message::user().text("remember that qq bots stay fixed to one agent"),
        });

        const auto result = loop.distill_session_memory();

        CHECK(result.distilled);
        CHECK(result.memories_stored == 3UL);
        CHECK(result.journal_stored);
        CHECK(provider.invocation_count() == 1UL);
        CHECK(provider.message_counts().front() == 1UL);
        CHECK(provider.tool_counts().front() == 0UL);
        CHECK(provider.prompts().front().contains("distilling long-term memory"));

        CHECK(store.recall("project.current", "agent:default|jid:test").contains("orangutan memory refactor"));
        CHECK(store.recall("decision.routing", "agent:default|jid:test").contains("fixed to one agent"));
        CHECK(store.recall("learning.runtime-identity", "agent:default|jid:test").contains("jid plus agent key"));
        const auto journals = store.list("agent:default|jid:test", "journal", 10);
        REQUIRE(journals.size() == 1UL);
        CHECK(journals.front().content.contains("markdown mirror behavior"));

        std::filesystem::remove_all(db_path.parent_path());
    }

    TEST_CASE("distill_session_memory_keeps_durable_memories_when_journal_line_is_malformed") {
        ScriptedProvider provider({testing::make_text_response("memory|project|project.current|0.90|orangutan memory refactor\n"
                                                               "journal")});
        ToolRegistry tools;

        const auto db_path = testing::unique_test_db_path("agent-loop-distill-partial-journal", "memory.db");
        MemoryStore store(db_path);
        RuntimeMemory runtime_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});
        AgentLoop loop(provider.system, provider.route, tools, &runtime_memory);
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
    }

    TEST_CASE("prompt_building_filters_journal_entries_based_on_user_intent") {
        const auto db_path = testing::unique_test_db_path("agent-loop-prompt-journal-filtering", "memory.db");
        MemoryStore store(db_path);
        store.remember("project.current", "orangutan memory enhancements", "project", memory_type::project, "agent:default|jid:test", "session:distilled", 0.9);
        store.remember("journal.1", "Yesterday we debugged the failing mirror refresh.", "journal", memory_type::project, "agent:default|jid:test", "session:journal",
                       0.4);
        RuntimeMemory runtime_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});
        ToolRegistry tools;

        ScriptedProvider ordinary_provider({testing::make_text_response("ok")});
        AgentLoop ordinary_loop(ordinary_provider.system, ordinary_provider.route, tools, &runtime_memory);
        static_cast<void>(ordinary_loop.run("what project am I working on?"));
        CHECK(ordinary_provider.prompts().front().contains("orangutan memory enhancements"));
        CHECK_FALSE(ordinary_provider.prompts().front().contains("Yesterday we debugged the failing mirror refresh."));

        ScriptedProvider journal_provider({testing::make_text_response("ok")});
        AgentLoop journal_loop(journal_provider.system, journal_provider.route, tools, &runtime_memory);
        static_cast<void>(journal_loop.run("what happened in the previous session journal?"));
        CHECK(journal_provider.prompts().front().contains("Yesterday we debugged the failing mirror refresh."));

        std::filesystem::remove_all(db_path.parent_path());
    }

    TEST_CASE("run_checkpoints_tool_flow_and_result_application") {
        ScriptedProvider provider({
            make_response(response_stop_reason::tool_use,
                          {
                              Text{"Looking that up."},
                              ToolUse("tool-1", "lookup", nlohmann::json{{"query", "status"}}),
                          }),
            testing::make_text_response("All set."),
        });

        ToolRegistry tools;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [](const nlohmann::json &) {
                    return std::string{"tool result"};
                },
        });

        AgentLoop loop(provider.system, provider.route, tools);
        const auto checkpoints = capture_checkpoint_descriptions(loop, "check status");

        CHECK(checkpoints == std::vector<std::vector<std::string>>{
                                 {"user:text=check status"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result"},
                                 {"user:text=check status", "assistant:text=Looking that up.|tool_use=lookup", "user:tool_result=tool result", "assistant:text=All set."},
                             });
    }

    TEST_CASE("run_inserts_continuation_prompt_before_continuation_call") {
        ScriptedProvider provider({
            make_response(response_stop_reason::max_tokens, {Text{"Part one. "}}),
            testing::make_text_response("Part two."),
        });
        ToolRegistry tools;
        AgentLoop loop(provider.system, provider.route, tools);

        const auto checkpoints = capture_checkpoint_descriptions(loop, "continue please");

        CHECK(checkpoints == std::vector<std::vector<std::string>>{
                                 {"user:text=continue please"},
                                 {"user:text=continue please", "assistant:text=Part one. "},
                                 {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off."},
                                 {"user:text=continue please", "assistant:text=Part one. ", "user:text=Please continue from where you left off.", "assistant:text=Part two."},
                             });
    }

    TEST_CASE("run_aborts_after_fifth_identical_tool_call") {
        ScriptedProvider provider({
            make_response(response_stop_reason::tool_use, {ToolUse("tool-1", "lookup", nlohmann::json{{"query", "status"}})}),
            make_response(response_stop_reason::tool_use, {ToolUse("tool-2", "lookup", nlohmann::json{{"query", "status"}})}),
            make_response(response_stop_reason::tool_use, {ToolUse("tool-3", "lookup", nlohmann::json{{"query", "status"}})}),
            make_response(response_stop_reason::tool_use, {ToolUse("tool-4", "lookup", nlohmann::json{{"query", "status"}})}),
            make_response(response_stop_reason::tool_use,
                          {
                              ToolUse("tool-5", "lookup", nlohmann::json{{"query", "status"}}),
                              ToolUse("tool-6", "side_effect", nlohmann::json::object()),
                          }),
        });

        ToolRegistry tools;
        int lookup_calls = 0;
        int side_effect_calls = 0;
        tools.register_tool({
            .definition = ToolDef{.name = "lookup", .description = "Lookup status", .input_schema = nlohmann::json::object()},
            .execute =
                [&lookup_calls](const nlohmann::json &) {
                    ++lookup_calls;
                    return std::string{"tool result"};
                },
        });
        tools.register_tool({
            .definition = ToolDef{.name = "side_effect", .description = "Side effect", .input_schema = nlohmann::json::object()},
            .execute =
                [&side_effect_calls](const nlohmann::json &) {
                    ++side_effect_calls;
                    return std::string{"should not run"};
                },
        });

        AgentLoop loop(provider.system, provider.route, tools);
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
    }

    TEST_CASE("run_refreshes_skill_prompt_after_conditional_activation") {
        const auto workspace = testing::unique_test_root("agent-loop-skill-prompt-refresh");
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

        ScriptedProvider provider({
            make_response(response_stop_reason::tool_use, {ToolUse("tool-read", "read", nlohmann::json{{"path", "src/main.cpp"}})}),
            testing::make_text_response("done"),
        });
        AgentLoop loop(provider.system, provider.route, tools, nullptr,
                       skills::render_skill_prompt_section(loader.list(skills::skill_list_query{.include_inactive = false})), nullptr, &loader);

        CHECK(loop.run("load file") == "done");
        const auto skill_invoke = loader.invoke({
            .name = "conditional",
            .call_origin = skills::skill_call_origin::automatic,
        });

        CHECK(skill_invoke.status == skills::skill_invoke_status::ok);
        REQUIRE(provider.prompts().size() == 2UL);
        CHECK_FALSE(provider.prompts()[0].contains("**conditional**"));
        CHECK(provider.prompts()[1].contains("**conditional**"));

        std::filesystem::remove_all(workspace);
    }

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
    }

    TEST_CASE("distill_session_memory_caps_distilled_memories_at_eight") {
        std::string distilled;
        for (int index = 0; index < 9; ++index) {
            distilled += "memory|project|project.item-" + std::to_string(index) + "|0.9|memory item " + std::to_string(index) + "\n";
        }

        ScriptedProvider provider({testing::make_text_response(distilled)});
        ToolRegistry tools;
        const auto db_path = testing::unique_test_db_path("agent-loop-distill-cap", "memory.db");
        MemoryStore store(db_path);
        RuntimeMemory runtime_memory(store, orangutan::bootstrap::RuntimeMemoryContext{.scope = "agent:default|jid:test"});
        AgentLoop loop(provider.system, provider.route, tools, &runtime_memory);
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
    }

    TEST_CASE("agent_loop_builder_creates_loop_with_fluent_api") {
        ScriptedProvider provider({testing::make_text_response("hello")});
        ToolRegistry tools;

        auto result = AgentLoop::configure(provider.system, provider.route, tools)
                          .with_thinking_budget(512)
                          .build();

        REQUIRE(result.has_value());
        auto reply = result->run("hi");
        CHECK(reply == "hello");
    }

    TEST_CASE("agent_loop_builder_with_stop_callback") {
        ScriptedProvider provider({testing::make_text_response("unused")});
        ToolRegistry tools;

        auto result = AgentLoop::configure(provider.system, provider.route, tools)
                          .with_stop_requested_callback([] { return true; })
                          .build();

        REQUIRE(result.has_value());
        auto reply = result->run("stop now");
        CHECK(reply == "Task terminated.");
    }

} // namespace
