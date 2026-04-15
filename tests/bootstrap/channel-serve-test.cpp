#include "bootstrap/channel-serve-runtime.hpp"
#include "bootstrap/channel-serve.hpp"
#include "bootstrap/app-runtime.hpp"
#include "bootstrap/identity.hpp"
#include "agent/agent-loop.hpp"
#include "channel/channel.hpp"
#include "channel/qq/qq-approval-keyboard.hpp"
#include "permissions/permission-state.hpp"
#include "permissions/permission-types.hpp"
#include "tools/background/background-completion.hpp"
#include "test-helpers.hpp"
#include "test-provider-support.hpp"

#include <concepts>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <catch2/catch_test_macros.hpp>
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
#include <variant>
#include <vector>

using namespace orangutan;

namespace {

    namespace bootstrap = orangutan::bootstrap;

    providers::ModelTarget make_runtime_target(std::string model, std::string api_key = "test-key", std::string base_url = "https://example.test",
                                               providers::provider_kind provider = providers::provider_kind::openai,
                                               providers::protocol_kind protocol = providers::protocol_kind::chat_completions) {
        return providers::ModelTarget{
            .profile_name = "test-profile",
            .model = std::move(model),
            .base_url = std::move(base_url),
            .api_key = std::move(api_key),
            .provider = provider,
            .protocol = protocol,
        };
    }

    providers::ProviderRoute make_runtime_route(std::string model, std::string api_key = "test-key", std::string base_url = "https://example.test",
                                                std::vector<providers::ModelTarget> fallbacks = {}) {
        return providers::ProviderRoute{
            .primary = make_runtime_target(std::move(model), std::move(api_key), std::move(base_url)),
            .fallbacks = std::move(fallbacks),
        };
    }

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
        struct TextMessage {
            std::string jid;
            std::string text;
            std::string reply_to_message_id;
            std::string reference_message_id;
        };

        struct KeyboardMessage {
            std::string jid;
            std::string markdown;
            nlohmann::json keyboard_payload;
            std::string reply_to_message_id;
            std::string reference_message_id;
        };

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

        void send(const std::string &jid, const OutboundMessage &message) override {
            std::scoped_lock lock(mutex_);
            std::visit(
                [&]<typename T>(const T &payload) {
                    using Payload = std::decay_t<T>;
                    if constexpr (std::same_as<Payload, TextPayload>) {
                        sent_text_messages_.push_back({
                            .jid = jid,
                            .text = payload.text,
                            .reply_to_message_id = message.reply_to_message_id,
                            .reference_message_id = message.reference_message_id,
                        });
                        sent_messages_.emplace_back(jid, payload.text);
                    } else if constexpr (std::same_as<Payload, MarkdownPayload>) {
                        sent_markdown_messages_.emplace_back(jid, payload.markdown);
                    } else if constexpr (std::same_as<Payload, KeyboardPayload>) {
                        ++keyboard_send_attempts_;
                        if (!keyboard_send_error_.empty()) {
                            throw std::runtime_error(keyboard_send_error_);
                        }
                        sent_keyboard_messages_.push_back({
                            .jid = jid,
                            .markdown = payload.markdown,
                            .keyboard_payload = payload.keyboard_payload,
                            .reply_to_message_id = message.reply_to_message_id,
                            .reference_message_id = message.reference_message_id,
                        });
                    } else {
                        throw std::runtime_error("FakeChannel only supports text/markdown/keyboard payloads");
                    }
                },
                message.payload);
            sent_reply_to_ids_.push_back(message.reply_to_message_id);
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

        void fail_keyboard_with(std::string message) {
            std::scoped_lock lock(mutex_);
            keyboard_send_error_ = std::move(message);
        }

        [[nodiscard]]
        std::size_t keyboard_send_attempts() const {
            std::scoped_lock lock(mutex_);
            return keyboard_send_attempts_;
        }

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> sent_messages() const {
            std::scoped_lock lock(mutex_);
            return sent_messages_;
        }

        [[nodiscard]]
        std::vector<TextMessage> sent_text_messages() const {
            std::scoped_lock lock(mutex_);
            return sent_text_messages_;
        }

        [[nodiscard]]
        std::vector<std::string> sent_reply_to_ids() const {
            std::scoped_lock lock(mutex_);
            return sent_reply_to_ids_;
        }

        [[nodiscard]]
        std::vector<std::pair<std::string, std::string>> sent_markdown_messages() const {
            std::scoped_lock lock(mutex_);
            return sent_markdown_messages_;
        }

        [[nodiscard]]
        std::vector<KeyboardMessage> sent_keyboard_messages() const {
            std::scoped_lock lock(mutex_);
            return sent_keyboard_messages_;
        }

    private:
        std::string name_;
        std::string jid_prefix_;
        MessageCallback on_message_;
        bool connected_ = false;
        mutable std::mutex mutex_;
        std::vector<std::pair<std::string, std::string>> sent_messages_;
        std::vector<TextMessage> sent_text_messages_;
        std::vector<std::pair<std::string, std::string>> sent_markdown_messages_;
        std::vector<KeyboardMessage> sent_keyboard_messages_;
        std::vector<std::string> sent_reply_to_ids_;
        std::string keyboard_send_error_;
        std::size_t keyboard_send_attempts_ = 0;
    };

    class ScriptedProvider {
    public:
        using Step = std::function<LLMResponse(const std::vector<Message> &)>;

        explicit ScriptedProvider(std::vector<Step> steps)
        : backend_(testing::make_fake_provider_backend([this](const providers::ProviderRoute &route, const providers::ProviderRequest &request,
                                                              const providers::ProviderEventSink &) {
              if (next_step_ >= steps_.size()) {
                  throw std::runtime_error("no scripted response available");
              }
              return providers::ProviderResult{
                  .response = steps_[next_step_++](request.messages),
                  .usage_snapshot = {},
                  .active_target = route.primary,
              };
          })),
          steps_(std::move(steps)),
          system(backend_),
          route(testing::make_test_route("gpt-test")) {
            backend_->set_label("scripted-provider");
        }

        std::shared_ptr<testing::FakeProviderBackend> backend_;
        std::vector<Step> steps_;
        std::size_t next_step_ = 0;
        providers::ProviderSystem system;
        providers::ProviderRoute route;
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

        ChannelServeHarness(const ChannelServeHarness &) = delete;
        ChannelServeHarness &operator=(const ChannelServeHarness &) = delete;
        ChannelServeHarness(ChannelServeHarness &&) = delete;
        ChannelServeHarness &operator=(ChannelServeHarness &&) = delete;

        static void write_skill(const std::filesystem::path &base_dir, const std::string &dir_name, const std::string &skill_name, const std::string &body) {
            const auto skill_dir = base_dir / dir_name;
            std::filesystem::create_directories(skill_dir);
            std::ofstream out(skill_dir / "SKILL.md");
            out << "---\n";
            out << "name: " << skill_name << "\n";
            out << "description: test skill\n";
            out << "---\n\n";
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
        static std::string extract_request_id(const FakeChannel::KeyboardMessage &message) {
            const auto &buttons = message.keyboard_payload.at("content").at("rows").at(0).at("buttons");
            if (buttons.empty()) {
                return {};
            }
            const auto parsed = channel::qq::parse_approval_callback_data(buttons.at(0).at("action").at("data").get<std::string>());
            return parsed.has_value() ? parsed->request_id : std::string{};
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

    TEST_CASE("resolves_agent_override_ahead_of_qq_routing") {
        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "ping",
            .agent_override = "assistant",
        };
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents{{"bot-a", "qq-agent"}};

        CHECK(bootstrap::resolve_agent_key_for_message(message, qq_bot_agents) == "assistant");
    };

    TEST_CASE("delivers_cli_replies_without_calling_channel_send") {
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

        CHECK(bootstrap::resolve_reply_target(message) == "cli");
        CHECK(completes_without_throw([&] {
            bootstrap::deliver_reply(message, "done", manager);
            bootstrap::deliver_command_reply(message, "Current agent: assistant", manager);
        }));
        CHECK(qq->sent_messages().empty());
        CHECK(ChannelServeHarness::contains_line(sink->lines(), "done"));
        CHECK(ChannelServeHarness::contains_line(sink->lines(), "Current agent: assistant"));
    };

    TEST_CASE("empty_reply_target_falls_back_to_cli") {
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

        CHECK(bootstrap::resolve_reply_target(message) == "cli");
        CHECK(completes_without_throw([&] {
            bootstrap::deliver_reply(message, "fallback", manager);
        }));
        CHECK(qq->sent_messages().empty());
        CHECK(ChannelServeHarness::contains_line(sink->lines(), "fallback"));
    };

    TEST_CASE("delivers_explicit_outbound_jid_through_owning_channel") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .message_id = "inbound-message-42",
            .reply_target = "qqbot:c2c:42",
        };

        CHECK(bootstrap::resolve_reply_target(message) == "qqbot:c2c:42");
        CHECK(completes_without_throw([&] {
            bootstrap::deliver_reply(message, "sent", manager);
        }));
        CHECK(qq->sent_messages().size() == 1UL);
        CHECK(qq->sent_messages()[0].first == "qqbot:c2c:42");
        CHECK(qq->sent_messages()[0].second == "sent");
        CHECK(qq->sent_reply_to_ids().size() == 1UL);
        CHECK(qq->sent_reply_to_ids()[0] == "inbound-message-42");
    };

    TEST_CASE("non_qq_reply_delivery_keeps_reply_to_without_reference") {
        ChannelManager manager;
        auto slack_channel = std::make_unique<FakeChannel>("slack", "slack:");
        auto *slack = slack_channel.get();
        manager.add_channel(std::move(slack_channel));

        const InboundMessage message{
            .jid = "slack:dm:42",
            .content = "prompt",
            .message_id = "slack-message-42",
        };

        CHECK(completes_without_throw([&] {
            bootstrap::deliver_reply(message, "sent", manager);
        }));

        const auto sent_messages = slack->sent_text_messages();
        REQUIRE(sent_messages.size() == 1UL);
        CHECK(sent_messages[0].jid == "slack:dm:42");
        CHECK(sent_messages[0].text == "sent");
        CHECK(sent_messages[0].reply_to_message_id == "slack-message-42");
        CHECK(sent_messages[0].reference_message_id.empty());
    };

    TEST_CASE("logs_unowned_outbound_jid_without_throwing") {
        auto sink = std::make_shared<MemorySink>();
        ScopedDefaultLogger logger("channel-serve-test", sink);
        ChannelManager manager;

        const InboundMessage message{
            .jid = "heartbeat:daily",
            .content = "prompt",
            .reply_target = "qqbot:c2c:missing",
        };

        CHECK(completes_without_throw([&] {
            bootstrap::deliver_reply(message, "still complete", manager);
        }));
        CHECK(ChannelServeHarness::contains_line(sink->lines(), "qqbot:c2c:missing"));
    };

    TEST_CASE("builds_skill_prompt_for_effective_agent_workspace") {
        ChannelServeHarness harness;
        ChannelServeHarness::write_skill(harness.home_root() / ".orangutan" / "skills", "home-skill", "home-skill", "Home skill body");
        ChannelServeHarness::write_skill(harness.workspace_root() / ".orangutan" / "skills", "workspace-skill", "workspace-skill", "Workspace skill body");

        ScopedEnvVar home_env("HOME", harness.home_root().string());

        Config cfg;
        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "assistant",
            .workspace_root = harness.workspace_root().string(),
        };

        const auto prompt = bootstrap::build_skill_prompt_for_runtime(cfg, runtime_cfg);
        CHECK(prompt.contains("## Available Skills"));
        CHECK(prompt.contains("**home-skill**"));
        CHECK(prompt.contains("**workspace-skill**"));
    };

    TEST_CASE("conversation_runtime_preserves_channel_context_and_shared_capabilities") {
        ChannelServeHarness harness;
        MemoryStore memory_store((harness.temp_root() / "memory.db"));

        Config cfg;
        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .fallback_models = {"gpt-fallback"},
            .provider_route = make_runtime_route("gpt-test", "test-key", "https://example.test", {make_runtime_target("gpt-fallback")}),
            .workspace_root = harness.workspace_root().string(),
        };

        const auto inspection = bootstrap::detail::inspect_conversation_runtime(cfg, runtime_cfg, &memory_store, nullptr, "qqbot:c2c:alice");

        CHECK(not(orangutan::testing::has_tool_named(inspection.tool_definitions, "memory_list")));
        CHECK(orangutan::testing::has_tool_named(inspection.tool_definitions, "tool_search"));
        CHECK(inspection.runtime_origin == base::origin::channel);
        CHECK(inspection.raw_caller_id == "qqbot:c2c:alice");
        CHECK(inspection.has_agent);
        CHECK(inspection.has_hook_manager);
        CHECK(inspection.session_scope_key == bootstrap::derive_channel_runtime_key("qqbot:c2c:alice", "default"));
        CHECK(inspection.configured_model == "gpt-test");
        CHECK(inspection.fallback_models.size() == 1UL);
        CHECK(inspection.fallback_models.front() == "gpt-fallback");
    };

    TEST_CASE("run_channel_loop_replies_when_runtime_creation_fails") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));

        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs;
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;
        Config cfg;

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr, nullptr, nullptr);
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
        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        const auto sent_messages = qq->sent_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages[0].first == "qqbot:c2c:42");
        CHECK(sent_messages[0].second.contains("Error: No runtime configuration for agent: missing"));
    };

    TEST_CASE("run_channel_loop_processes_reaction_events_instead_of_ignoring_them") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));

        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs;
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;
        Config cfg;

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr, nullptr, nullptr);
        });

        queue.push(InboundMessage{
            .event_kind = inbound_event_kind::reaction_added,
            .jid = "qqbot:guild:42",
            .sender = "user-1",
            .sender_name = "user-1",
            .message_id = "msg-1",
            .reaction =
                ReactionInfo{
                    .user_id = "user-1",
                    .target_id = "msg-1",
                    .target_type = 0,
                    .emoji_id = "128077",
                    .emoji_type = 1,
                },
            .is_group = true,
        });

        for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stop_requested.store(true);
        queue.shutdown();
        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

        const auto sent_messages = qq->sent_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages[0].first == "qqbot:guild:42");
        CHECK(sent_messages[0].second.contains("Error: No runtime configuration for agent: default"));
    };

    TEST_CASE("channel_approval_coordinator_prompts_and_accepts_replies") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages[0].jid == "qqbot:c2c:42");
        CHECK(sent_messages[0].reply_to_message_id.empty());
        CHECK(sent_messages[0].reference_message_id.empty());
        CHECK(sent_messages[0].markdown.contains("## 🔐 Command Execution Approval"));
        CHECK(sent_messages[0].markdown.contains("> Shell command approval required."));
        CHECK(sent_messages[0].markdown.contains("echo hello"));
        CHECK(sent_messages[0].markdown.contains("🧰 Tool: `shell`"));
        CHECK_FALSE(sent_messages[0].markdown.contains("Request:"));
        CHECK_FALSE(sent_messages[0].markdown.contains("If the QQ buttons fail"));
        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0]);
        CHECK_FALSE(request_id.empty());
        CHECK(sent_messages[0].keyboard_payload.at("content").at("rows").at(0).at("buttons").size() == 3UL);

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::allow_once)}, manager));
        CHECK(future.get());
    };

    TEST_CASE("channel_approval_coordinator_formats_text_prompts_and_parses_text_always_replies") {
        ChannelManager manager;
        auto irc_channel = std::make_unique<FakeChannel>("irc", "irc:");
        auto *irc = irc_channel.get();
        manager.add_channel(std::move(irc_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "irc:dm:42",
            .content = "run shell",
            .message_id = "message-42",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && irc->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = irc->sent_text_messages();
        REQUIRE(sent_messages.size() == 1UL);
        CHECK(sent_messages[0].jid == "irc:dm:42");
        CHECK(sent_messages[0].reply_to_message_id == "message-42");
        CHECK(sent_messages[0].reference_message_id.empty());
        CHECK(sent_messages[0].text.contains("Shell command approval required."));
        CHECK(sent_messages[0].text.contains("Tool: shell"));
        CHECK(sent_messages[0].text.contains("Command: echo hello"));
        CHECK(sent_messages[0].text.contains("Please reply with `"));
        CHECK(sent_messages[0].text.contains("yes`, `"));
        CHECK(sent_messages[0].text.contains("always`, or `"));
        CHECK(sent_messages[0].text.contains("no`."));

        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0].text);
        REQUIRE_FALSE(request_id.empty());

        CHECK(coordinator.handle_inbound_message(InboundMessage{.jid = "irc:dm:42", .content = "  " + request_id + " ALWAYS allow!!!  "}, manager));
        CHECK(future.get());
    };

    TEST_CASE("channel_approval_coordinator_invalid_text_replies_do_not_reference_qq_buttons") {
        ChannelManager manager;
        auto irc_channel = std::make_unique<FakeChannel>("irc", "irc:");
        auto *irc = irc_channel.get();
        manager.add_channel(std::move(irc_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "irc:dm:42",
            .content = "run shell",
            .message_id = "message-42",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && irc->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto initial_messages = irc->sent_text_messages();
        REQUIRE(initial_messages.size() == 1UL);
        const auto request_id = ChannelServeHarness::extract_request_id(initial_messages[0].text);
        REQUIRE_FALSE(request_id.empty());

        CHECK(coordinator.handle_inbound_message(InboundMessage{.jid = "irc:dm:42", .content = "what?"}, manager));
        CHECK(future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);

        const auto sent_messages = irc->sent_text_messages();
        REQUIRE(sent_messages.size() >= 2UL);
        CHECK_FALSE(sent_messages.back().text.contains("QQ buttons"));
        CHECK(sent_messages.back().text.contains(request_id + " yes"));
        CHECK(sent_messages.back().text.contains(request_id + " no"));

        CHECK(coordinator.handle_inbound_message(InboundMessage{.jid = "irc:dm:42", .content = request_id + " no"}, manager));
        CHECK_FALSE(future.get());
    };

    TEST_CASE("channel_approval_coordinator_rejects_when_qq_keyboard_is_unavailable") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        qq->fail_keyboard_with("QQ API POST /v2/users/42/messages failed: http=500, trace_id=test-trace, biz_code=304057, biz_message=not allowd custom keyborad");
        manager.add_channel(std::move(qq_channel));

        auto permission_context = initialize_permission_context(PermissionConfig{});
        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager, nullptr, [&permission_context](PermissionRule rule) {
            permission_context = add_rule(permission_context, rule);
        });
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto first_prompts = qq->sent_messages();
        REQUIRE(not first_prompts.empty());
        CHECK(first_prompts.front().second.contains("QQ approval buttons are unavailable"));
        CHECK(first_prompts.front().second.contains("tool call was rejected"));
        CHECK(qq->keyboard_send_attempts() == 1UL);
        CHECK_FALSE(future.get());
        CHECK(permission_context.allow_rules.empty());

        const auto prompt_count_before = qq->sent_messages().size();
        auto second_future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("deny-shell", "shell", nlohmann::json{{"command", "pwd"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_messages().size() == prompt_count_before; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto second_prompts = qq->sent_messages();
        REQUIRE(second_prompts.size() > prompt_count_before);
        CHECK(qq->keyboard_send_attempts() == 1UL);
        CHECK(second_prompts.back().second.contains("QQ approval buttons are unavailable"));
        CHECK_FALSE(second_future.get());
    };

    TEST_CASE("channel_approval_coordinator_persists_allow_always_rules_when_session_exists") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        SessionStore session_store((harness.temp_root() / "sessions.db"));
        auto permission_context = initialize_permission_context(PermissionConfig{});
        auto session_id = session_store.create_empty(SessionMetadata{
            .model = "gpt-test",
            .scope_key = "scope:test",
            .agent_key = "default",
            .origin_kind = "channel",
            .origin_ref = "qqbot:c2c:42",
        });

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
            .message_id = "msg-1",
        };
        auto callback = coordinator.make_callback(request, manager, nullptr, [&session_store, &session_id, &permission_context](PermissionRule rule) {
            permission_context = add_rule(permission_context, rule);
            session_store.save_session_permission_rule(session_id, std::move(rule));
        });
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages[0].reply_to_message_id.empty());
        CHECK(sent_messages[0].reference_message_id.empty());
        CHECK(sent_messages[0].markdown.contains("## 🔐 Command Execution Approval"));
        CHECK(sent_messages[0].markdown.contains("echo hello"));
        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0]);
        REQUIRE_FALSE(request_id.empty());

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::always_allow)}, manager));
        CHECK(future.get());
        REQUIRE(permission_context.allow_rules.size() == std::size_t{1});
        CHECK(permission_context.allow_rules[0].source == permission_rule_source::session);
        CHECK(permission_context.allow_rules[0].tool_name == "shell");
        REQUIRE(permission_context.allow_rules[0].content.has_value());
        if (permission_context.allow_rules[0].content.has_value()) {
            CHECK(permission_context.allow_rules[0].content->pattern == "echo hello");
        }

        const auto stored_rules = session_store.load_session_permission_rules(session_id);
        REQUIRE(stored_rules.size() == std::size_t{1});
        CHECK(stored_rules[0].source == permission_rule_source::session);
        CHECK(stored_rules[0].tool_name == "shell");
    };

    TEST_CASE("channel_approval_coordinator_includes_decision_reason_details_in_qq_card") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "git push origin main"}}),
                            PermissionDecision::ask_by_rule(permission_rule_source::project_settings, "shell(git push *)", "Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages[0].markdown.contains("git push origin main"));
        CHECK(sent_messages[0].markdown.contains("🧰 Tool: `shell`"));
        CHECK(sent_messages[0].markdown.contains("📝 Reason: `rule from project settings`"));
        CHECK(sent_messages[0].markdown.contains("📏 Rule: `shell(git push *)`"));

        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0]);
        REQUIRE_FALSE(request_id.empty());
        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::allow_once)}, manager));
        CHECK(future.get());
    };

    TEST_CASE("channel_approval_coordinator_omits_allow_always_button_for_compound_shell_commands") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello && pwd"}}),
                            PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        const auto buttons = sent_messages[0].keyboard_payload.at("content").at("rows").at(0).at("buttons");
        REQUIRE(buttons.size() == 2UL);
        CHECK(buttons.at(0).at("render_data").at("label").get<std::string>() == "Allow once");
        CHECK(buttons.at(1).at("render_data").at("label").get<std::string>() == "Deny");

        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0]);
        REQUIRE_FALSE(request_id.empty());
        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::always_allow)}, manager));
        auto fallback_messages = qq->sent_messages();
        REQUIRE(not fallback_messages.empty());
        CHECK(fallback_messages.back().second.contains("Always allow is not available for this request"));

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::deny)}, manager));
        CHECK_FALSE(future.get());
    };

    TEST_CASE("channel_approval_coordinator_omits_allow_always_button_when_tool_signature_is_not_eligible") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "search tools",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("tool-search", "tool_search", nlohmann::json{{"query", "find shell tools"}}),
                            PermissionDecision::ask_default("Tool search approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        const auto buttons = sent_messages[0].keyboard_payload.at("content").at("rows").at(0).at("buttons");
        REQUIRE(buttons.size() == 2UL);
        CHECK(buttons.at(0).at("render_data").at("label").get<std::string>() == "Allow once");
        CHECK(buttons.at(1).at("render_data").at("label").get<std::string>() == "Deny");

        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages[0]);
        REQUIRE_FALSE(request_id.empty());
        CHECK(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:42", .content = request_id + " no"}, manager));
        CHECK_FALSE(future.get());
    };

    TEST_CASE("channel_approval_coordinator_consumes_invalid_replies_while_waiting") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:99",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("deny-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_keyboard_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_keyboard_messages.empty());
        const auto request_id = ChannelServeHarness::extract_request_id(sent_keyboard_messages.front());
        CHECK_FALSE(request_id.empty());

        CHECK(coordinator.handle_inbound_message(InboundMessage{.jid = "qqbot:c2c:99", .content = request_id + " yes"}, manager));
        CHECK(future.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
        auto sent_messages = qq->sent_messages();
        CHECK(sent_messages.size() >= 1UL);
        CHECK(sent_messages.back().second.contains("Use the buttons on the approval card"));

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:99", .content = channel::qq::build_approval_callback_data("shell-approval-999", channel::qq::approval_action::deny)}, manager));
        sent_messages = qq->sent_messages();
        CHECK(sent_messages.size() >= 2UL);
        CHECK(sent_messages.back().second.contains("Use the buttons on the approval card"));

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:99", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::deny)}, manager));
        CHECK_FALSE(future.get());
    };

    TEST_CASE("channel_approval_coordinator_accepts_allow_always_callback_payload") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        auto future = std::async(std::launch::async, [&callback] {
            return callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required."));
        });

        for (int attempt = 0; attempt < 20 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto sent_messages = qq->sent_keyboard_messages();
        REQUIRE(not sent_messages.empty());
        const auto request_id = ChannelServeHarness::extract_request_id(sent_messages.front());
        REQUIRE_FALSE(request_id.empty());

        CHECK(coordinator.handle_inbound_message(
            InboundMessage{.jid = "qqbot:c2c:42", .content = channel::qq::build_approval_callback_data(request_id, channel::qq::approval_action::always_allow)}, manager));
        CHECK(future.get());
    };

    TEST_CASE("channel_approval_coordinator_disables_prompts_when_replies_cannot_return_to_same_conversation") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));

        CHECK(coordinator.make_callback(
                  InboundMessage{
                      .jid = "heartbeat:nightly",
                      .reply_target = "qqbot:c2c:42",
                  },
                  manager) == nullptr);

        CHECK(coordinator.make_callback(
                  InboundMessage{
                      .jid = "qqbot:c2c:42",
                      .reply_target = "qqbot:c2c:other",
                  },
                  manager) == nullptr);
    };

    TEST_CASE("approval_wait_does_not_starve_other_jids_and_shutdown_cancels_it") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        JidTaskRunner runner(1);
        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::seconds(5));
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
            approval_result.store(
                callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required.")));
            approval_finished.set_value();
        });

        runner.submit("qqbot:c2c:99", [&] {
            bob_started.set_value();
        });

        for (int attempt = 0; attempt < 50 && qq->sent_keyboard_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(not qq->sent_keyboard_messages().empty());
        CHECK(bob_started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);

        coordinator.shutdown();
        CHECK(approval_finished_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK_FALSE(approval_result.load());

        const auto sent_messages = qq->sent_keyboard_messages();
        CHECK(sent_messages.size() == 1UL);
        CHECK_FALSE(sent_messages.front().markdown.contains("Request:"));
        CHECK_FALSE(ChannelServeHarness::extract_request_id(sent_messages.front()).empty());

        runner.shutdown(true);
    };

    TEST_CASE("channel_approval_coordinator_rejects_callbacks_after_shutdown") {
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::ChannelApprovalCoordinator coordinator(std::chrono::milliseconds(250));
        const InboundMessage request{
            .jid = "qqbot:c2c:42",
            .content = "run shell",
        };
        auto callback = coordinator.make_callback(request, manager);
        REQUIRE(callback != nullptr);

        coordinator.shutdown();

        CHECK_FALSE(callback(ToolUse("approve-shell", "shell", nlohmann::json{{"command", "echo hello"}}), PermissionDecision::ask_default("Shell command approval required.")));
        CHECK(qq->sent_messages().empty());
        CHECK(qq->sent_keyboard_messages().empty());
        CHECK(coordinator.make_callback(request, manager) == nullptr);
    };

    TEST_CASE("new_command_updates_bound_session_with_channel_metadata") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        Config cfg;

        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .provider_route = make_runtime_route("gpt-test"),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = bootstrap::derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id =
            session_store.save({Message::user().text("hello"), Message::assistant().text("hi")},
                               orangutan::SessionMetadata{.model = "gpt-test", .scope_key = identity.runtime_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        session_store.bind_jid(jid, session_id, "default");

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr, nullptr, nullptr);
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

        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        REQUIRE(not qq->sent_messages().empty());
        CHECK(qq->sent_messages().front().second == "## Session\n- ✨ Started a new session.");

        const auto sessions = session_store.list_sessions_for_agent("default");
        CHECK(sessions.size() == 1UL);
        CHECK(sessions[0].id == session_id);
        CHECK(sessions[0].scope_key == identity.runtime_key);
        CHECK(sessions[0].agent_key == "default");
        CHECK(sessions[0].origin_kind == "channel");
        CHECK(sessions[0].origin_ref == jid);
    };

    TEST_CASE("export_command_writes_transcript_to_workspace_and_replies_with_path") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        Config cfg;

        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .provider_route = make_runtime_route("gpt-test"),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = bootstrap::derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id =
            session_store.save({Message::user().text("hello"), Message(base::role::assistant, {ToolUse("1", "read", nlohmann::json::object())})},
                               orangutan::SessionMetadata{.model = "gpt-test", .scope_key = identity.runtime_key, .agent_key = "", .origin_kind = "cli", .origin_ref = ""});
        session_store.bind_jid(jid, session_id, "default");
        const auto export_path = bootstrap::workspace_exports_root(identity.workspace) / (session_id + ".md");

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr, nullptr, nullptr);
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

        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        REQUIRE(not qq->sent_messages().empty());
        CHECK(qq->sent_messages().front().second == "## Export\n- Saved current session to `" + export_path.string() + '`');
        CHECK(std::filesystem::exists(export_path));

        std::ifstream in(export_path);
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content.contains("# Session Export"));
        CHECK(content.contains("hello"));
        CHECK(content.contains("### Tool Use: `read`"));
    };

    TEST_CASE("resume_command_trims_session_id_whitespace") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        Config cfg;

        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .provider_route = make_runtime_route("gpt-test"),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;

        const std::string jid = "qqbot:c2c:42";
        const auto identity = bootstrap::derive_channel_identity(harness.workspace_root().string(), jid, "default");
        const auto session_id = session_store.save({Message::user().text("hello")}, SessionMetadata{
                                                                                        .model = "gpt-test",
                                                                                        .scope_key = identity.runtime_key,
                                                                                        .agent_key = "default",
                                                                                        .origin_kind = "channel",
                                                                                        .origin_ref = jid,
                                                                                    });

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr, nullptr, nullptr);
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

        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        REQUIRE(not qq->sent_messages().empty());
        CHECK(qq->sent_messages().front().second == "🧵 Resumed session: " + session_id);
    };

    TEST_CASE("automation_command_replies_with_automation_tool_output") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);
        bootstrap::AppRuntime app_runtime(harness.temp_root() / "automation.db");
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        Config cfg;

        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "gpt-test",
            .provider_route = make_runtime_route("gpt-test"),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, nullptr, session_store, nullptr, cfg, nullptr,
                                        &app_runtime.automation_runtime());
        });

        queue.push(InboundMessage{
            .jid = "qqbot:c2c:42",
            .content = "/automation",
        });

        for (int attempt = 0; attempt < 50 && qq->sent_messages().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        stop_requested.store(true);
        queue.shutdown();

        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        REQUIRE(not qq->sent_messages().empty());
        CHECK(qq->sent_messages().front().second == "## Automation\n- No automations configured.");
    };

    TEST_CASE("completion_resume_persists_bound_session_and_replies_through_channel") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        bootstrap::AppRuntime app_runtime(harness.temp_root() / "automation.db");
        SessionStore session_store((harness.temp_root() / "sessions.db"));

        const std::string jid = "qqbot:c2c:42";
        const auto identity = bootstrap::derive_channel_identity(harness.workspace_root().string(), jid, "default");
        std::vector<Message> history = {
            Message::user().text("hello"),
            Message::assistant().text("hi"),
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

                CHECK(messages.back().role() == base::role::user);
                const auto *text = messages.back().begin() == messages.back().end() ? nullptr : std::get_if<Text>(&*messages.back().begin());
                CHECK((text != nullptr));
                if (text != nullptr) {
                    CHECK(text->text.contains("\"type\": \"background_process_completion\""));
                }

                LLMResponse response;
                response.stop_reason = response_stop_reason::end_turn;
                response.content.emplace_back(Text{"Background reply"});
                return response;
            },
        });
        AgentLoop agent(provider.system, provider.route, tools);
        agent.set_history(history);

        auto resume_state = std::make_shared<bootstrap::detail::ChannelCompletionResumeState>();
        resume_state->agent = &agent;
        resume_state->provider = &provider.system;
        std::size_t persisted_message_count = history.size();
        std::string current_session_id = session_id;
        resume_state->current_session_id = &current_session_id;
        resume_state->persisted_message_count = &persisted_message_count;
        resume_state->session_store = &session_store;
        resume_state->channel_manager = &manager;
        resume_state->jid = jid;
        resume_state->agent_key = "default";
        resume_state->configured_model = "gpt-test";
        resume_state->session_scope_key = identity.runtime_key;
        resume_state->automation_runtime = &app_runtime.automation_runtime();

        ToolRuntimeContext tool_context{
            .runtime_key = identity.runtime_key,
            .agent_key = "default",
            .scope_key = identity.runtime_key,
            .automation_service = &app_runtime.automation_service(),
            .automation_runtime = &app_runtime.automation_runtime(),
            .background_completion_runtime = make_background_completion_runtime_bindings(
                [&app_runtime](const automation::DeliveryRecord &delivery) {
                    static_cast<void>(app_runtime.automation_service().record_delivery(delivery));
                },
                bootstrap::detail::make_channel_completion_resume_callback(resume_state)),
        };
        tools::BackgroundCompletionDispatcher dispatcher(&tool_context);

        dispatcher.dispatch(BackgroundProcessCompletionEvent{
            .process_id = "proc-channel",
            .command = "printf 'done\\n'",
            .working_dir = harness.workspace_root().string(),
            .pid = 1234,
            .terminal_status = background_process_terminal_status::exited,
            .exit_code = 0,
            .stdout = {.tail = "done\n", .total_bytes = 5, .truncated = false},
            .metadata = {{std::string(tools::BACKGROUND_COMPLETION_MODE_METADATA_KEY), "resume"}},
        });

        const auto deliveries = app_runtime.automation_service().list_deliveries(automation::DeliveryQuery{.agent_key = "default"});
        CHECK(deliveries.size() == 1UL);
        REQUIRE(not qq->sent_messages().empty());
        CHECK(qq->sent_messages().front().first == jid);
        CHECK(qq->sent_messages().front().second == "Background reply");

        const auto persisted_history = session_store.load(session_id);
        CHECK(current_session_id == session_id);
        CHECK(persisted_message_count == persisted_history.size());
        CHECK(persisted_history.size() >= 4UL);
        CHECK(persisted_history.back().role() == base::role::assistant);
        const auto *reply_text = persisted_history.back().begin() == persisted_history.back().end() ? nullptr : std::get_if<Text>(&*persisted_history.back().begin());
        REQUIRE(reply_text != nullptr);
        if (reply_text != nullptr) {
            CHECK(reply_text->text == "Background reply");
        }

        const auto bound_session = session_store.bound_session_for_jid(jid, "default");
        REQUIRE(bound_session.has_value());
        if (bound_session.has_value()) {
            CHECK(*bound_session == session_id);
        }
    };

    TEST_CASE("completion_resume_callback_reports_unavailable_runtime_after_state_expires") {
        const auto callback = [&] {
            auto resume_state = std::make_shared<bootstrap::detail::ChannelCompletionResumeState>();
            auto callback = bootstrap::detail::make_channel_completion_resume_callback(resume_state);
            resume_state.reset();
            return callback;
        }();

        const auto error = callback("ignored");
        REQUIRE(error.has_value());
        CHECK(*error == "channel runtime is no longer available");
    };

    TEST_CASE("run_channel_loop_replies_with_runtime_errors") {
        ChannelServeHarness harness;
        ChannelManager manager;
        auto qq_channel = std::make_unique<FakeChannel>("qqbot", "qqbot:");
        auto *qq = qq_channel.get();
        manager.add_channel(std::move(qq_channel));

        MessageQueue queue;
        std::atomic<bool> stop_requested{false};
        JidTaskRunner task_runner(1);

        const bootstrap::AgentRuntimeConfig runtime_cfg{
            .agent_key = "default",
            .model = "broken-model",
            .provider_route = make_runtime_route("broken-model", ""),
            .workspace_root = harness.workspace_root().string(),
        };
        const std::unordered_map<std::string, bootstrap::AgentRuntimeConfig> agent_configs{{"default", runtime_cfg}};
        const orangutan::utils::transparent_string_unordered_map<std::string> qq_bot_agents;

        MemoryStore memory_store((harness.temp_root() / "memory.db"));
        SessionStore session_store((harness.temp_root() / "sessions.db"));
        Config cfg;

        auto loop = std::async(std::launch::async, [&] {
            bootstrap::run_channel_loop(queue, manager, stop_requested, task_runner, agent_configs, qq_bot_agents, &memory_store, session_store, nullptr, cfg, nullptr, nullptr,
                                        nullptr);
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

        CHECK(loop.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        const auto sent_messages = qq->sent_messages();
        REQUIRE(not sent_messages.empty());
        CHECK(sent_messages.back().first == "qqbot:c2c:42");
        CHECK(sent_messages.back().second.contains("Error:"));
        CHECK(sent_messages.back().second.contains("missing api key"));
    };

} // namespace
