#include "app/channel-serve.hpp"
#include "app/runtime/identity.hpp"
#include "features/agent/agent-loop.hpp"
#include "features/automation/runtime.hpp"
#include "features/automation/store.hpp"
#include "features/channel/core/channel.hpp"
#include "features/tools/core/background-completion.hpp"
#include "test-helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include "support/ut.hpp"
#include <memory>
#include <mutex>
#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace orangutan;

namespace {

template <typename Fn>
bool completes_without_throw(Fn &&fn) {
    try {
        std::forward<Fn>(fn)();
        return true;
    } catch (...) {
        return false;
    }
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

using ScopedDefaultLogger = orangutan::testing::ScopedDefaultLogger<MemorySink>;

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

class ScriptedProvider final : public Provider {
public:
    using Step = std::function<LLMResponse(const std::vector<Message> &)>;

    explicit ScriptedProvider(std::vector<Step> steps)
    : steps_(std::move(steps)) {}

    LLMResponse chat(const std::string &, const std::vector<Message> &, const std::vector<ToolDef> &, int) override {
        throw std::runtime_error("chat should not be used in this test");
    }

    LLMResponse chat_stream(const std::string &, const std::vector<Message> &messages, const std::vector<ToolDef> &, const StreamCallback &, int) override {
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
    size_t next_step_ = 0;
};

using ScopedEnvVar = orangutan::testing::ScopedEnvVar;

class ChannelServeHarness {
public:
    ChannelServeHarness()
    : temp_root_(orangutan::testing::unique_test_root("channel-serve")),
      home_root_(temp_root_ / "home"),
      workspace_root_(temp_root_ / "workspace") {
        std::filesystem::create_directories(home_root_);
        std::filesystem::create_directories(workspace_root_);
    }

    ~ChannelServeHarness() {
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

boost::ut::suite channel_serve_suite = [] {
    using namespace boost::ut;

    "resolves_agent_override_ahead_of_qq_routing"_test = [] {
        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "ping",
            .agent_override = "assistant",
        };
        const std::unordered_map<std::string, std::string> qq_bot_agents{{"bot-a", "qq-agent"}};

        expect(app::resolve_agent_key_for_message(message, qq_bot_agents) == "assistant");
    };

    "delivers_cli_reply_without_calling_channel_send"_test = [] {
        auto sink = std::make_shared<MemorySink>();
        ScopedDefaultLogger logger("channel-serve-test", sink);
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .reply_target = "cli",
        };

        expect(app::resolve_reply_target(message) == "cli");
        expect(completes_without_throw([&] {
            app::deliver_reply(message, "done", manager);
        }));
        expect(qq->sent_messages().empty());
        expect(ChannelServeHarness::contains_line(sink->lines(), "done"));
    };

    "empty_reply_target_falls_back_to_cli"_test = [] {
        auto sink = std::make_shared<MemorySink>();
        ScopedDefaultLogger logger("channel-serve-test", sink);
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .reply_target = "",
        };

        expect(app::resolve_reply_target(message) == "cli");
        expect(completes_without_throw([&] {
            app::deliver_reply(message, "fallback", manager);
        }));
        expect(qq->sent_messages().empty());
        expect(ChannelServeHarness::contains_line(sink->lines(), "fallback"));
    };

    "delivers_explicit_outbound_jid_through_owning_channel"_test = [] {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .reply_target = "qqbot:c2c:42",
        };

        expect(app::resolve_reply_target(message) == "qqbot:c2c:42");
        expect(completes_without_throw([&] {
            app::deliver_reply(message, "sent", manager);
        }));
        expect(qq->sent_messages().size() == 1_ul);
        expect(qq->sent_messages()[0].first == "qqbot:c2c:42");
        expect(qq->sent_messages()[0].second == "sent");
    };

    "logs_unowned_outbound_jid_without_throwing"_test = [] {
        auto sink = std::make_shared<MemorySink>();
        ScopedDefaultLogger logger("channel-serve-test", sink);
        ChannelManager manager;

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .reply_target = "qqbot:c2c:missing",
        };

        expect(completes_without_throw([&] {
            app::deliver_reply(message, "still complete", manager);
        }));
        expect(ChannelServeHarness::contains_line(sink->lines(), "qqbot:c2c:missing"));
    };

    "builds_skill_prompt_for_effective_agent_workspace"_test = [] {
        ChannelServeHarness harness;
        ChannelServeHarness::write_skill(harness.home_root() / ".orangutan" / "skills", "home-skill", "home-skill", "Home skill body");
        ChannelServeHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");

        ScopedEnvVar home_env("HOME", harness.home_root().string());

        Config cfg;
        const app::AgentRuntimeConfig runtime_cfg{
            .agent_key = "assistant",
            .system_prompt = "You are the assistant.",
            .workspace_root = harness.workspace_root().string(),
        };

        const auto prompt = app::build_skill_prompt_for_runtime(cfg, runtime_cfg);
        expect(prompt.find("## Active Skills") != std::string::npos);
        expect(prompt.find("### home-skill") != std::string::npos);
        expect(prompt.find("### workspace-skill") != std::string::npos);
    };

    "conversation_runtime_preserves_channel_context_and_shared_capabilities"_test = [] {
        ChannelServeHarness harness;
        MemoryStore memory_store((harness.temp_root() / "memory.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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
            .workspace_root = harness.workspace_root().string(),
        };

        const auto inspection = app::detail::inspect_conversation_runtime(cfg, runtime_cfg, &memory_store, subagent_manager, "qqbot:c2c:alice");

        expect(orangutan::testing::has_tool_named(inspection.tool_definitions, "memory_list"));
        expect(inspection.runtime_origin == SubagentRuntimeOrigin::channel);
        expect(inspection.raw_caller_id == "qqbot:c2c:alice");
        expect(inspection.has_agent);
        expect(inspection.has_hook_manager);
        expect(inspection.session_scope_key == derive_channel_runtime_key("qqbot:c2c:alice", "default"));
        expect(inspection.configured_model == "gpt-test");
        expect(inspection.fallback_models.size() == 1_ul);
        expect(inspection.fallback_models.front() == "gpt-fallback");
    };

    "delivers_command_reply_to_cli_without_calling_channel_send"_test = [] {
        auto sink = std::make_shared<MemorySink>();
        ScopedDefaultLogger logger("channel-serve-test", sink);
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "/agent",
            .reply_target = "cli",
        };

        expect(completes_without_throw([&] {
            app::deliver_command_reply(message, "Current agent: assistant", manager);
        }));
        expect(qq->sent_messages().empty());
        expect(ChannelServeHarness::contains_line(sink->lines(), "Current agent: assistant"));
    };

    "run_channel_loop_replies_when_runtime_creation_fails"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "subagent-runs.db"));
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
        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        const auto sent_messages = qq->sent_messages();
        expect(not sent_messages.empty() >> fatal);
        expect(sent_messages[0].first == "qqbot:c2c:42");
        expect(sent_messages[0].second.find("Error: No runtime configuration for agent: missing") != std::string::npos);
    };

    "channel_approval_coordinator_prompts_and_accepts_replies"_test = [] {
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
        expect(static_cast<bool>(callback) >> fatal);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUseBlock{.id = "approve-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required.");
        });

        for (int attempt = 0; attempt < 20 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_messages();
        expect(not sent_messages.empty() >> fatal);
        expect(sent_messages[0].first == "qqbot:c2c:42");
        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0].second);
        expect(not request_id.empty());
        expect(sent_messages[0].second.find(request_id + " yes") != std::string::npos);

        expect(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:42", .content = request_id + " yes"}, manager));
        expect(future.get());
    };

    "channel_approval_coordinator_consumes_invalid_replies_while_waiting"_test = [] {
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
        expect(static_cast<bool>(callback) >> fatal);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUseBlock{.id = "deny-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required.");
        });

        for (int attempt = 0; attempt < 20 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        auto sent_messages = qq->sent_messages();
        expect(not sent_messages.empty() >> fatal);
        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages.front().second);
        expect(not request_id.empty());

        expect(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = "maybe"}, manager));
        sent_messages = qq->sent_messages();
        expect(sent_messages.size() >= 2_ul);
        expect(sent_messages.back().second.find(request_id + " yes") != std::string::npos);

        expect(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = "shell-approval-999 no"}, manager));
        sent_messages = qq->sent_messages();
        expect(sent_messages.size() >= 3_ul);
        expect(sent_messages.back().second.find("Shell approval is pending") != std::string::npos);

        expect(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = request_id + " no"}, manager));
        expect(not future.get());
    };

    "channel_approval_coordinator_disables_prompts_when_replies_cannot_return_to_same_conversation"_test = [] {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        manager.add_channel(std::move(qq_channel));

        app::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));

        expect(not static_cast<bool>(coordinator.make_callback(
            InboundMessage{
                .jid = "heartbeat:nightly",
                .reply_target = "qqbot:c2c:42",
            },
            manager)));

        expect(not static_cast<bool>(coordinator.make_callback(
            InboundMessage{
                .jid = "qqbot:c2c:42",
                .reply_target = "qqbot:c2c:other",
            },
            manager)));
    };

    "approval_wait_does_not_starve_other_jids_and_shutdown_cancels_it"_test = [] {
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

        expect(not qq->sent_messages().empty() >> fatal);
        expect(bob_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

        coordinator.shutdown();
        expect(approval_finished_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        expect(not approval_result.load());

        const auto sent_messages = qq->sent_messages();
        expect(sent_messages.size() == 1_ul);
        expect(sent_messages.front().second.find("Request: shell-approval-") != std::string::npos);

        runner.shutdown(true);
    };

    "channel_approval_coordinator_rejects_callbacks_after_shutdown"_test = [] {
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
        expect(static_cast<bool>(callback) >> fatal);

        coordinator.shutdown();

        expect(not callback(ToolUseBlock{.id = "approve-shell", .name = "shell", .input = {{"command", "echo hello"}}}, "Shell command approval required."));
        expect(qq->sent_messages().empty());
        expect(not static_cast<bool>(coordinator.make_callback(request, manager)));
    };

    "new_command_updates_bound_session_with_channel_metadata"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const std::unordered_map<std::string, std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id =
            session_store.save({Message::user_text("hello"), Message::assistant_text("hi")},
                               orangutan::SessionMetadata{.model = "gpt-test", .scope_key = identity.runtime_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
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

        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(not qq->sent_messages().empty() >> fatal);
        expect(qq->sent_messages().front().second == "## Session\n- ✨ Started a new session.");

        const auto sessions = session_store.list_sessions_for_agent("default");
        expect(sessions.size() == 1_ul);
        expect(sessions[0].id == session_id);
        expect(sessions[0].scope_key == identity.runtime_key);
        expect(sessions[0].agent_key == "default");
        expect(sessions[0].origin_kind == "channel");
        expect(sessions[0].origin_ref == jid);
    };

    "export_command_writes_transcript_to_workspace_and_replies_with_path"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const std::unordered_map<std::string, std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id =
            session_store.save({Message::user_text("hello"), {.role = "assistant", .content = {ToolUseBlock{.id = "1", .name = "read", .input = json::object()}}}},
                               orangutan::SessionMetadata{.model = "gpt-test", .scope_key = identity.runtime_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        session_store.bind_jid(jid, session_id, "default");
        const auto export_path = std::filesystem::path(identity.workspace) / ".exports" / (session_id + ".md");

        auto loop = std::async(std::launch::async, [&] {
            app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, subagent_manager, cfg);
        });

        queue.push(InboundMessage{
            .jid = jid,
            .content = "/export",
        });

        for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stop_requested.store(true);
        queue.shutdown();

        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(not qq->sent_messages().empty() >> fatal);
        expect(qq->sent_messages().front().second == "## Export\n- Saved current session to `" + export_path.string() + '`');
        expect(std::filesystem::exists(export_path));

        std::ifstream in(export_path);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        expect(content.find("# Session Export") != std::string::npos);
        expect(content.find("hello") != std::string::npos);
        expect(content.find("### Tool Use: `read`") != std::string::npos);
    };

    "resume_command_trims_session_id_whitespace"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const std::unordered_map<std::string, std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id = session_store.save({Message::user_text("hello")}, SessionMetadata{
                                                                                      .model = "gpt-test",
                                                                                      .scope_key = identity.runtime_key,
                                                                                      .agent_key = "default",
                                                                                      .origin_kind = "channel",
                                                                                      .origin_ref = jid,
                                                                                  });

        auto loop = std::async(std::launch::async, [&] {
            app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, subagent_manager, cfg);
        });

        queue.push(InboundMessage{
            .jid = jid,
            .content = "/resume    latest   ",
        });

        for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stop_requested.store(true);
        queue.shutdown();

        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(not qq->sent_messages().empty() >> fatal);
        expect(qq->sent_messages().front().second == "🧵 Resumed session: " + session_id);
    };

    "tasks_command_replies_with_task_tool_output"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        auto automation_store = std::make_shared<automation::Store>((harness.temp_root() / "automation.db"));
        automation::Runtime automation_runtime(*automation_store);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const std::unordered_map<std::string, std::string> qq_bot_agents;

        auto loop = std::async(std::launch::async, [&] {
            app::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, subagent_manager, cfg, nullptr,
                                  &automation_runtime);
        });

        queue.push(InboundMessage{
            .jid = "qqbot:c2c:42",
            .content = "/tasks",
        });

        for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stop_requested.store(true);
        queue.shutdown();

        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        expect(not qq->sent_messages().empty() >> fatal);
        expect(qq->sent_messages().front().second == "## Tasks\n- 🗓️ No tasks configured.");
    };

    "completion_resume_persists_bound_session_and_replies_through_channel"_test = [] {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        auto automation_store = std::make_shared<automation::Store>((harness.temp_root() / "automation.db"));
        automation::Runtime automation_runtime(*automation_store);
        SessionStore session_store((harness.temp_root() / "sessions.db"));

        const std::string jid = "qqbot:c2c:42";
        const auto identity = derive_channel_identity(harness.workspace_root().string(), jid, "default");
        std::vector<Message> history = {
            Message::user_text("hello"),
            Message::assistant_text("hi"),
        };
        const auto session_id = session_store.save(
            history, orangutan::SessionMetadata{.model = "gpt-test", .scope_key = identity.runtime_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        session_store.bind_jid(jid, session_id, "default");

        ToolRegistry tools;
        ScriptedProvider provider({
            [](const std::vector<Message> &messages) {
                if (messages.empty()) {
                    return LLMResponse{};
                }

                boost::ut::expect(messages.back().role == "user");
                const auto *text = messages.back().content.empty() ? nullptr : std::get_if<TextBlock>(&messages.back().content.front());
                boost::ut::expect((text != nullptr) >> boost::ut::fatal);
                if (text != nullptr) {
                    boost::ut::expect(text->text.find("\"type\": \"background_process_completion\"") != std::string::npos);
                }

                LLMResponse response;
                response.stop_reason = "end_turn";
                response.content.emplace_back(TextBlock{.text = "Background reply"});
                return response;
            },
        });
        AgentLoop agent(provider, tools, "You are a test agent.");
        agent.set_history(history);

        auto resume_state = std::make_shared<app::detail::ChannelCompletionResumeState>();
        resume_state->agent = &agent;
        resume_state->provider = &provider;
        size_t persisted_message_count = history.size();
        std::string current_session_id = session_id;
        resume_state->current_session_id = &current_session_id;
        resume_state->persisted_message_count = &persisted_message_count;
        resume_state->session_store = &session_store;
        resume_state->channel_manager = &manager;
        resume_state->jid = jid;
        resume_state->agent_key = "default";
        resume_state->configured_model = "gpt-test";
        resume_state->session_scope_key = identity.runtime_key;
        resume_state->automation_runtime = &automation_runtime;

        ToolRuntimeContext tool_context{
            .runtime_key = identity.runtime_key,
            .agent_key = "default",
            .scope_key = identity.runtime_key,
            .automation_runtime = &automation_runtime,
            .background_completion_runtime = make_background_completion_runtime_bindings(
                [automation_store](const automation::InboxItem &item) {
                    static_cast<void>(automation_store->insert_inbox(item));
                },
                app::detail::make_channel_completion_resume_callback(resume_state)),
        };
        BackgroundCompletionDispatcher dispatcher(&tool_context);

        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-channel",
            .command = "printf 'done\\n'",
            .working_dir = harness.workspace_root().string(),
            .pid = 1234,
            .terminal_status = BackgroundProcessTerminalStatus::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(background_completion_mode_metadata_key), "resume"}},
        });

        const auto inbox_items = automation_runtime.list_inbox("default");
        expect(inbox_items.size() == 1_ul);
        expect(not qq->sent_messages().empty() >> fatal);
        expect(qq->sent_messages().front().first == jid);
        expect(qq->sent_messages().front().second == "Background reply");

        const auto persisted_history = session_store.load(session_id);
        expect(current_session_id == session_id);
        expect(persisted_message_count == persisted_history.size());
        expect(persisted_history.size() >= 4_ul);
        expect(persisted_history.back().role == "assistant");
        const auto *reply_text = persisted_history.back().content.empty() ? nullptr : std::get_if<TextBlock>(&persisted_history.back().content.front());
        expect((reply_text != nullptr) >> fatal);
        if (reply_text != nullptr) {
            expect(reply_text->text == "Background reply");
        }

        const auto bound_session = session_store.bound_session_for_jid(jid, "default");
        expect(bound_session.has_value() >> fatal);
        if (bound_session.has_value()) {
            expect(*bound_session == session_id);
        }
    };

    "run_channel_loop_replies_with_runtime_errors"_test = [] {
        ChannelServeHarness harness;
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
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, app::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const std::unordered_map<std::string, std::string> qq_bot_agents;

        MemoryStore memory_store((harness.temp_root() / "memory.db"));
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        SubagentRunStore run_store((harness.temp_root() / "runs.db"));
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

        expect(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto sent_messages = qq->sent_messages();
        expect(not sent_messages.empty() >> fatal);
        expect(sent_messages.back().first == "qqbot:c2c:42");
        expect(sent_messages.back().second.find("Error:") != std::string::npos);
        expect(sent_messages.back().second.find("Unknown provider") != std::string::npos);
    };
};

} // namespace
