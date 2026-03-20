#include "app/channel-serve.hpp"
#include "app/runtime/identity.hpp"
#include "features/channel/core/channel.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace orangutan;

namespace {

bool has_tool_named(const std::vector<ToolDef> &definitions, const std::string &name) {
    return std::ranges::any_of(definitions, [&name](const ToolDef &definition) {
        return definition.name == name;
    });
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

class ScopedDefaultLogger {
public:
    explicit ScopedDefaultLogger(const std::shared_ptr<MemorySink> &sink)
    : previous_(spdlog::default_logger()),
      previous_level_(spdlog::get_level()) {
        logger_ = std::make_shared<spdlog::logger>("channel-serve-test", sink);
        logger_->set_pattern("%l %v");
        spdlog::set_default_logger(logger_);
        spdlog::set_level(spdlog::level::debug);
    }

    ~ScopedDefaultLogger() {
        spdlog::set_default_logger(previous_);
        spdlog::set_level(previous_level_);
    }

    ScopedDefaultLogger(const ScopedDefaultLogger &) = delete;
    ScopedDefaultLogger &operator=(const ScopedDefaultLogger &) = delete;
    ScopedDefaultLogger(ScopedDefaultLogger &&) = delete;
    ScopedDefaultLogger &operator=(ScopedDefaultLogger &&) = delete;

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> previous_;
    spdlog::level::level_enum previous_level_;
};

class FakeChannel final : public Channel {
public:
    FakeChannel(std::string channel_name, std::string jid_prefix)
    : name_(std::move(channel_name)),
      jid_prefix_(std::move(jid_prefix)) {}

    std::string name() const override {
        return name_;
    }

    void connect(MessageCallback on_message) override {
        connected_ = true;
        on_message_ = std::move(on_message);
    }

    void send_message(const std::string &jid, const std::string &text) override {
        std::scoped_lock lock(mutex_);
        sent_messages_.emplace_back(jid, text);
    }

    void disconnect() override {
        connected_ = false;
    }

    bool owns_jid(const std::string &jid) const override {
        return jid.starts_with(jid_prefix_);
    }

    bool is_connected() const override {
        return connected_;
    }

    [[nodiscard]]
    std::vector<std::pair<std::string, std::string>> sent_messages() const {
        std::scoped_lock lock(mutex_);
        return sent_messages_;
    }

private:
    std::string name_;
    std::string jid_prefix_;
    MessageCallback on_message_;
    bool connected_ = false;
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> sent_messages_;
};

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

class ChannelServeTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_root_ = std::filesystem::current_path() / "tmp" / "tests" / "orangutan_channel_serve_test";
        home_root_ = temp_root_ / "home";
        workspace_root_ = temp_root_ / "workspace";
        std::filesystem::remove_all(temp_root_);
        std::filesystem::create_directories(home_root_);
        std::filesystem::create_directories(workspace_root_);
    }

    void TearDown() override {
        std::filesystem::remove_all(temp_root_);
    }

    static void write_skill(const std::filesystem::path &base_dir, const std::string &dir_name, const std::string &skill_name, const std::string &body) {
        const auto skill_dir = base_dir / dir_name;
        std::filesystem::create_directories(skill_dir);
        std::ofstream out(skill_dir / "SKILL.md");
        out << "+++\n";
        out << "name = \"" << skill_name << "\"\n";
        out << "description = \"test skill\"\n";
        out << "+++\n\n";
        out << body << "\n";
    }

    [[nodiscard]]
    static bool contains_line(const std::vector<std::string> &lines, const std::string &needle) {
        return std::ranges::any_of(lines, [&needle](const std::string &line) {
            return line.contains(needle);
        });
    }

    [[nodiscard]]
    static std::string extract_request_id(const std::string &message) {
        const auto marker = std::string("Request: ");
        const auto start = message.find(marker);
        if (start == std::string::npos) {
            return {};
        }

        const auto value_start = start + marker.size();
        const auto value_end = message.find('\n', value_start);
        return message.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
    }

    [[nodiscard]]
    const std::filesystem::path &temp_root() const {
        return temp_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &home_root() const {
        return home_root_;
    }

    [[nodiscard]]
    const std::filesystem::path &workspace_root() const {
        return workspace_root_;
    }

private:
    std::filesystem::path temp_root_;
    std::filesystem::path home_root_;
    std::filesystem::path workspace_root_;
};

TEST_F(ChannelServeTest, ResolvesAgentOverrideAheadOfQqRouting) {
    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "ping",
        .agent_override = "assistant",
    };
    const std::unordered_map<std::string, std::string> qq_bot_agents{{"bot-a", "qq-agent"}};

    EXPECT_EQ(app::resolve_agent_key_for_message(message, qq_bot_agents), "assistant");
}

TEST_F(ChannelServeTest, DeliversCliReplyWithoutCallingChannelSend) {
    auto sink = std::make_shared<MemorySink>();
    ScopedDefaultLogger logger(sink);
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "prompt",
        .reply_target = "cli",
    };

    EXPECT_EQ(app::resolve_reply_target(message), "cli");
    EXPECT_NO_THROW(app::deliver_reply(message, "done", manager));
    EXPECT_TRUE(qq->sent_messages().empty());
    EXPECT_TRUE(contains_line(sink->lines(), "done"));
}

TEST_F(ChannelServeTest, EmptyReplyTargetFallsBackToCli) {
    auto sink = std::make_shared<MemorySink>();
    ScopedDefaultLogger logger(sink);
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "prompt",
        .reply_target = "",
    };

    EXPECT_EQ(app::resolve_reply_target(message), "cli");
    EXPECT_NO_THROW(app::deliver_reply(message, "fallback", manager));
    EXPECT_TRUE(qq->sent_messages().empty());
    EXPECT_TRUE(contains_line(sink->lines(), "fallback"));
}

TEST_F(ChannelServeTest, DeliversExplicitOutboundJidThroughOwningChannel) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "prompt",
        .reply_target = "qqbot:c2c:42",
    };

    EXPECT_EQ(app::resolve_reply_target(message), "qqbot:c2c:42");
    EXPECT_NO_THROW(app::deliver_reply(message, "sent", manager));
    ASSERT_EQ(qq->sent_messages().size(), 1);
    EXPECT_EQ(qq->sent_messages()[0].first, "qqbot:c2c:42");
    EXPECT_EQ(qq->sent_messages()[0].second, "sent");
}

TEST_F(ChannelServeTest, LogsUnownedOutboundJidWithoutThrowing) {
    auto sink = std::make_shared<MemorySink>();
    ScopedDefaultLogger logger(sink);
    ChannelManager manager;

    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "prompt",
        .reply_target = "qqbot:c2c:missing",
    };

    EXPECT_NO_THROW(app::deliver_reply(message, "still complete", manager));
    EXPECT_TRUE(contains_line(sink->lines(), "qqbot:c2c:missing"));
}

TEST_F(ChannelServeTest, BuildsSkillPromptForEffectiveAgentWorkspace) {
    write_skill(home_root() / ".orangutan" / "skills", "home-skill", "home-skill", "Home skill body");
    write_skill(workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");

    ScopedEnvVar home_env("HOME", home_root().string());

    Config cfg;
    const app::AgentRuntimeConfig runtime_cfg{
        .agent_key = "assistant",
        .system_prompt = "You are the assistant.",
        .workspace_root = workspace_root().string(),
    };

    const auto prompt = app::build_skill_prompt_for_runtime(cfg, runtime_cfg);
    EXPECT_NE(prompt.find("## Active Skills"), std::string::npos);
    EXPECT_NE(prompt.find("### home-skill"), std::string::npos);
    EXPECT_NE(prompt.find("### workspace-skill"), std::string::npos);
}

TEST_F(ChannelServeTest, ConversationRuntimePreservesChannelContextAndSharedCapabilities) {
    MemoryStore memory_store((temp_root() / "memory.db").string());
    SubagentRunStore run_store((temp_root() / "runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    Config cfg;
    const app::AgentRuntimeConfig runtime_cfg{
        .agent_key = "default",
        .provider_name = "openai",
        .api_key = "test-key",
        .model = "gpt-test",
        .fallback_models = {"gpt-fallback"},
        .base_url = "https://example.test",
        .system_prompt = "You are a test agent.",
        .workspace_root = workspace_root().string(),
    };

    const auto inspection = app::detail::inspect_conversation_runtime(cfg, runtime_cfg, &memory_store, subagent_manager, "qqbot:c2c:alice");

    EXPECT_TRUE(has_tool_named(inspection.tool_definitions, "memory_list"));
    EXPECT_EQ(inspection.runtime_origin, SubagentRuntimeOrigin::channel);
    EXPECT_EQ(inspection.raw_caller_id, "qqbot:c2c:alice");
    EXPECT_TRUE(inspection.has_agent);
    EXPECT_TRUE(inspection.has_hook_manager);
    EXPECT_EQ(inspection.session_scope_key, derive_channel_runtime_key("qqbot:c2c:alice", "default"));
    EXPECT_EQ(inspection.configured_model, "gpt-test");
    ASSERT_EQ(inspection.fallback_models.size(), 1U);
    EXPECT_EQ(inspection.fallback_models.front(), "gpt-fallback");
}

TEST_F(ChannelServeTest, DeliversCommandReplyToCliWithoutCallingChannelSend) {
    auto sink = std::make_shared<MemorySink>();
    ScopedDefaultLogger logger(sink);
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    const InboundMessage message{
        .jid = "heartbeat:daily",
        .content = "/agent",
        .reply_target = "cli",
    };

    EXPECT_NO_THROW(app::deliver_command_reply(message, "Current agent: assistant", manager));
    EXPECT_TRUE(qq->sent_messages().empty());
    EXPECT_TRUE(contains_line(sink->lines(), "Current agent: assistant"));
}

TEST_F(ChannelServeTest, RunChannelLoopRepliesWhenRuntimeCreationFails) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    MessageQueue queue;
    std::atomic<bool> stop_requested{false};
    JidTaskRunner task_runner(1);
    SessionStore session_store((temp_root() / "sessions.db").string());
    SubagentRunStore run_store((temp_root() / "subagent-runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });

    const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs;
    const std::unordered_map<std::string, std::string> qq_bot_agents;
    Config cfg;

    auto loop = std::async(std::launch::async, [&] {
        app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, subagent_manager, cfg);
    });

    queue.push(InboundMessage{
        .jid = "qqbot:c2c:42",
        .content = "hello",
        .agent_override = "missing",
    });

    for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_requested.store(true);
    queue.shutdown();
    ASSERT_EQ(loop.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    const auto sent_messages = qq->sent_messages();
    ASSERT_FALSE(sent_messages.empty());
    EXPECT_EQ(sent_messages[0].first, "qqbot:c2c:42");
    EXPECT_NE(sent_messages[0].second.find("Error: No runtime configuration for agent: missing"), std::string::npos);
}

TEST_F(ChannelServeTest, ChannelApprovalCoordinatorPromptsAndAcceptsReplies) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    app::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
    const InboundMessage request{
        .jid = "qqbot:c2c:42",
        .content = "run shell",
    };
    auto callback = coordinator.make_callback(request, manager);
    ASSERT_TRUE(static_cast<bool>(callback));

    auto future = std::async(std::launch::async, [&callback] {
        return callback(ToolUseBlock{.id = "approve-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required.");
    });

    for (int attempt = 0; attempt < 20 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto sent_messages = qq->sent_messages();
    ASSERT_FALSE(sent_messages.empty());
    EXPECT_EQ(sent_messages[0].first, "qqbot:c2c:42");
    const auto request_id = extract_request_id(sent_messages[0].second);
    EXPECT_FALSE(request_id.empty());
    EXPECT_NE(sent_messages[0].second.find(request_id + " yes"), std::string::npos);

    EXPECT_TRUE(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:42", .content = request_id + " yes"}, manager));
    EXPECT_TRUE(future.get());
}

TEST_F(ChannelServeTest, ChannelApprovalCoordinatorConsumesInvalidRepliesWhileWaiting) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    app::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
    const InboundMessage request{
        .jid = "qqbot:c2c:99",
        .content = "run shell",
    };
    auto callback = coordinator.make_callback(request, manager);
    ASSERT_TRUE(static_cast<bool>(callback));

    auto future = std::async(std::launch::async, [&callback] {
        return callback(ToolUseBlock{.id = "deny-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required.");
    });

    for (int attempt = 0; attempt < 20 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto sent_messages = qq->sent_messages();
    ASSERT_FALSE(sent_messages.empty());
    const auto request_id = extract_request_id(sent_messages.front().second);
    ASSERT_FALSE(request_id.empty());

    EXPECT_TRUE(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = "maybe"}, manager));
    sent_messages = qq->sent_messages();
    ASSERT_GE(sent_messages.size(), 2U);
    EXPECT_NE(sent_messages.back().second.find(request_id + " yes"), std::string::npos);

    EXPECT_TRUE(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = "shell-approval-999 no"}, manager));
    sent_messages = qq->sent_messages();
    ASSERT_GE(sent_messages.size(), 3U);
    EXPECT_NE(sent_messages.back().second.find("Shell approval is pending"), std::string::npos);

    EXPECT_TRUE(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = request_id + " no"}, manager));
    EXPECT_FALSE(future.get());
}

TEST_F(ChannelServeTest, ChannelApprovalCoordinatorDisablesPromptsWhenRepliesCannotReturnToSameConversation) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    manager.add_channel(std::move(qq_channel));

    app::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));

    EXPECT_FALSE(static_cast<bool>(coordinator.make_callback(
        InboundMessage{
            .jid = "heartbeat:nightly",
            .reply_target = "qqbot:c2c:42",
        },
        manager)));

    EXPECT_FALSE(static_cast<bool>(coordinator.make_callback(
        InboundMessage{
            .jid = "qqbot:c2c:42",
            .reply_target = "qqbot:c2c:other",
        },
        manager)));
}

TEST_F(ChannelServeTest, ApprovalWaitDoesNotStarveOtherJidsAndShutdownCancelsIt) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    JidTaskRunner runner(1);
    app::ChannelApprovalCoordinator coordinator(std::chrono::seconds(5));
    const InboundMessage approval_request{
        .jid = "qqbot:c2c:42",
        .content = "run shell",
    };

    std::promise<void> bob_started;
    auto bob_started_future = bob_started.get_future();
    std::promise<void> approval_finished;
    auto approval_finished_future = approval_finished.get_future();
    std::atomic<bool> approval_result = true;

    runner.submit(approval_request.jid, [&] {
        auto callback = coordinator.make_callback(approval_request, manager, &runner);
        approval_result.store(callback(ToolUseBlock{.id = "approve-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required."));
        approval_finished.set_value();
    });

    runner.submit("qqbot:c2c:99", [&] {
        bob_started.set_value();
    });

    for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ASSERT_FALSE(qq->sent_messages().empty());
    ASSERT_EQ(bob_started_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);

    coordinator.shutdown();
    ASSERT_EQ(approval_finished_future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(approval_result.load());

    const auto sent_messages = qq->sent_messages();
    ASSERT_EQ(sent_messages.size(), 1U);
    EXPECT_NE(sent_messages.front().second.find("Request: shell-approval-"), std::string::npos);

    runner.shutdown(true);
}

TEST_F(ChannelServeTest, ChannelApprovalCoordinatorRejectsCallbacksAfterShutdown) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    app::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
    const InboundMessage request{
        .jid = "qqbot:c2c:42",
        .content = "run shell",
    };
    auto callback = coordinator.make_callback(request, manager);
    ASSERT_TRUE(static_cast<bool>(callback));

    coordinator.shutdown();

    EXPECT_FALSE(callback(ToolUseBlock{.id = "approve-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required."));
    EXPECT_TRUE(qq->sent_messages().empty());
    EXPECT_FALSE(static_cast<bool>(coordinator.make_callback(request, manager)));
}

TEST_F(ChannelServeTest, NewCommandUpdatesBoundSessionWithChannelMetadata) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    MessageQueue queue;
    std::atomic<bool> stop_requested{false};
    JidTaskRunner task_runner(1);
    SessionStore session_store((temp_root() / "sessions.db").string());
    SubagentRunStore run_store((temp_root() / "runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    Config cfg;

    const app::AgentRuntimeConfig runtime_cfg{
        .agent_key = "default",
        .provider_name = "openai",
        .api_key = "test-key",
        .model = "gpt-test",
        .base_url = "https://example.test",
        .system_prompt = "You are a test agent.",
        .workspace_root = workspace_root().string(),
    };
    const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
    const std::unordered_map<std::string, std::string> qq_bot_agents;

    const std::string jid = "qqbot:c2c:42";
    const auto identity = derive_channel_identity(workspace_root().string(), jid, "default");
    const auto session_id = session_store.save({Message::user_text("hello"), Message::assistant_text("hi")}, "gpt-test", identity.runtime_key);
    session_store.bind_jid(jid, session_id, "default");

    auto loop = std::async(std::launch::async, [&] {
        app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, subagent_manager, cfg);
    });

    queue.push(InboundMessage{
        .jid = jid,
        .content = "/new",
    });

    for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_requested.store(true);
    queue.shutdown();

    ASSERT_EQ(loop.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    ASSERT_FALSE(qq->sent_messages().empty());

    const auto sessions = session_store.list_sessions_for_agent("default");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].id, session_id);
    EXPECT_EQ(sessions[0].scope_key, identity.runtime_key);
    EXPECT_EQ(sessions[0].agent_key, "default");
    EXPECT_EQ(sessions[0].origin_kind, "channel");
    EXPECT_EQ(sessions[0].origin_ref, jid);
}

TEST_F(ChannelServeTest, RunChannelLoopRepliesWithRuntimeErrors) {
    ChannelManager manager;
    auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
    auto *qq = qq_channel.get();
    manager.add_channel(std::move(qq_channel));

    MessageQueue queue;
    std::atomic<bool> stop_requested{false};
    JidTaskRunner task_runner(1);

    const app::AgentRuntimeConfig runtime_cfg{
        .agent_key = "default",
        .provider_name = "unknown-provider",
        .api_key = "test-key",
        .model = "broken-model",
        .workspace_root = workspace_root().string(),
    };
    const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
    const std::unordered_map<std::string, std::string> qq_bot_agents;

    MemoryStore memory_store((temp_root() / "memory.db").string());
    SessionStore session_store((temp_root() / "sessions.db").string());
    SubagentRunStore run_store((temp_root() / "runs.db").string());
    SubagentManager subagent_manager(run_store, [](const SubagentWorkerRequest &) {
        return SubagentWorkerResult{.status = SubagentRunStatus::succeeded};
    });
    Config cfg;

    auto loop = std::async(std::launch::async, [&] {
        app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, &memory_store, session_store, subagent_manager, cfg);
    });

    queue.push(InboundMessage{
        .jid = "qqbot:c2c:42",
        .content = "hello",
    });

    for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_requested.store(true);
    queue.shutdown();

    ASSERT_EQ(loop.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    const auto sent_messages = qq->sent_messages();
    ASSERT_FALSE(sent_messages.empty());
    EXPECT_EQ(sent_messages.back().first, "qqbot:c2c:42");
    EXPECT_NE(sent_messages.back().second.find("Error:"), std::string::npos);
    EXPECT_NE(sent_messages.back().second.find("Unknown provider"), std::string::npos);
}

} // namespace
