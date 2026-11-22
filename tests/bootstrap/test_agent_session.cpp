#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/agent.hpp>
#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/channel.hpp>
#include <oran/config.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/hook.hpp>
#include <oran/memory.hpp>
#include <oran/permission.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace memory = orangutan::memory;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

struct ProviderHookCapture {
  hook::Event event;
  hook::Payload payload;
};

struct MemoryHookCapture {
  hook::Event event;
  hook::Payload payload;
};

struct CapturingEventSink final : provider::EventSink {
  std::size_t text_deltas{0};
  std::size_t done_calls{0};
  std::string text;

  void on_text_delta(std::string_view delta) override {
    ++text_deltas;
    text += std::string{delta};
  }
  void on_done(core::StopReason /*stop_reason*/) override {
    ++done_calls;
  }
};

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto out = std::ofstream{path};
  out << contents;
}

provider::Route test_route() {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "fake",
              .model = "fake-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
          },
      .fallbacks = {},
  };
}

provider::Response text_response(std::string text) {
  return provider::Response{
      .blocks = {core::TextContent{.text = std::move(text)}},
      .stop_reason = core::StopReason::end_turn,
      .usage = provider::Usage{.input_tokens = 3,
                               .output_tokens = 2,
                               .cache_creation_tokens = 0,
                               .cache_read_tokens = 0,
                               .cost_estimate = std::nullopt},
      .model_used = std::string{"fake-1"},
  };
}

provider::Response tool_response(std::string name, std::string id, std::string input) {
  return provider::Response{
      .blocks = {core::ToolUseContent{.id = std::move(id), .name = std::move(name), .input_json = std::move(input)}},
      .stop_reason = core::StopReason::tool_use,
      .usage = {},
      .model_used = std::string{"fake-1"},
  };
}

std::string tool_result_output_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      if (result != nullptr && result->tool_use_id == tool_use_id) {
        return result->output;
      }
    }
  }
  return {};
}

std::optional<std::string> tool_result_data_json_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      if (result != nullptr && result->tool_use_id == tool_use_id) {
        return result->data_json;
      }
    }
  }
  return std::nullopt;
}

config::Config parse_config(std::string_view json) {
  auto parsed = config::Config::parse(json);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

bootstrap::RuntimeAssembly build_assembly(const std::filesystem::path& workspace,
                                          asio::any_io_executor executor,
                                          bool audit_enabled,
                                          bool session_memory_enabled = false,
                                          bool longterm_memory_enabled = true) {
  auto options = bootstrap::RuntimeAssemblyOptions{};
  options.audit_enabled = audit_enabled;
  options.session_memory_enabled = session_memory_enabled;
  options.longterm_memory_enabled = longterm_memory_enabled;
  auto assembly = bootstrap::RuntimeAssembly::build(workspace.string(), std::move(executor), std::move(options));
  REQUIRE(assembly.has_value());
  return std::move(*assembly);
}

bootstrap::RuntimeAssembly build_assembly(const std::filesystem::path& workspace,
                                          asio::io_context& io,
                                          bool audit_enabled,
                                          bool session_memory_enabled = false,
                                          bool longterm_memory_enabled = true) {
  return build_assembly(workspace, io.get_executor(), audit_enabled, session_memory_enabled, longterm_memory_enabled);
}

memory::longterm::Record make_longterm_record(std::string id, std::string body) {
  const auto created = core::Time{core::Time::time_point{std::chrono::seconds{1}}};
  const auto updated = core::Time{core::Time::time_point{std::chrono::seconds{2}}};
  return memory::longterm::Record{
      .key = memory::longterm::RecordKey{.id = std::move(id), .scope_key = "scope-A"},
      .kind = memory::longterm::RecordKind::project,
      .title = "Recall note",
      .body = std::move(body),
      .created_at = created,
      .updated_at = updated,
      .last_read_at = updated,
      .importance = 0.8,
      .tags = {"recall"},
      .linked_record_ids = {},
  };
}

bootstrap::AgentSessionOptions base_runner_options(asio::any_io_executor executor,
                                                   bootstrap::RuntimeAssembly& assembly,
                                                   config::Config& cfg,
                                                   provider::System& provider_system) {
  auto options = bootstrap::AgentSessionOptions{};
  options.blocking_executor = executor;
  options.executor = std::move(executor);
  options.assembly = &assembly;
  options.config = &cfg;
  options.provider = &provider_system;
  options.route = test_route();
  options.scope_key = "scope-A";
  options.agent_key = "coder";
  options.identity = "operator-1";
  options.origin = "test";
  return options;
}

bootstrap::AgentSessionOptions base_runner_options(asio::io_context& io,
                                                   bootstrap::RuntimeAssembly& assembly,
                                                   config::Config& cfg,
                                                   provider::System& provider_system) {
  return base_runner_options(io.get_executor(), assembly, cfg, provider_system);
}

hook::Sink provider_capture_sink(std::vector<ProviderHookCapture>& captures) {
  return hook::Sink{
      .id = "provider-capture",
      .observe = [&captures](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<void> {
        captures.push_back(ProviderHookCapture{.event = event, .payload = *payload});
        co_return;
      },
  };
}

class RecordingProvider final : public provider::System {
public:
  explicit RecordingProvider(std::vector<provider::Response> responses)
      : responses_{std::make_move_iterator(responses.begin()), std::make_move_iterator(responses.end())} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget, provider::EventSink* sink = nullptr) const override {
    {
      const std::lock_guard lock{mutex_};
      requests_.push_back(std::move(request));
      if (responses_.empty()) {
        co_return std::unexpected(core::Error::internal("recording provider plan exhausted"));
      }
    }

    provider::Response response;
    {
      const std::lock_guard lock{mutex_};
      response = std::move(responses_.front());
      responses_.pop_front();
    }
    if (sink != nullptr) {
      sink->on_done(response.stop_reason);
    }
    co_return response;
  }

  [[nodiscard]] std::vector<provider::Request> requests() const {
    const std::lock_guard lock{mutex_};
    return requests_;
  }

private:
  mutable std::mutex mutex_;
  mutable std::vector<provider::Request> requests_;
  mutable std::deque<provider::Response> responses_;
};

/// Trusted sink that records every memory observation and gate request.
hook::Sink memory_capture_sink(std::vector<MemoryHookCapture>& captures,
                               hook::HookDecision blocking_decision = hook::HookDecision{}) {
  return hook::Sink{
      .id = "memory-capture",
      .observe = [&captures](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<void> {
        captures.push_back(MemoryHookCapture{.event = event, .payload = *payload});
        co_return;
      },
      .decide = [&captures, blocking_decision](hook::Event event, hook::PayloadPtr payload)
          -> async::Awaitable<core::Result<hook::HookDecision>> {
        captures.push_back(MemoryHookCapture{.event = event, .payload = *payload});
        co_return blocking_decision;
      },
      .trusted_local = true,
  };
}

}  // namespace

TEST_CASE("AgentSession rejects an empty executor at create time", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-empty-executor"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.executor = asio::any_io_executor{};

    auto runner = bootstrap::AgentSession::create(std::move(options));

    REQUIRE_FALSE(runner.has_value());
    REQUIRE(runner.error().kind() == core::ErrorKind::invalid_argument);
    co_return;
  });
}

TEST_CASE("AgentSession drives prompts through the agent loop and trace writer", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-trace"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, true);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("runner ok"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());

    auto request = agent::PromptRequest{.prompt = "hello"};
    auto result = co_await (*runner)->run_prompt(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(fake.turns_consumed() == 1);
    REQUIRE(assembly.trace_repository() != nullptr);
    auto turns = co_await assembly.trace_repository()->list_turns({});
    REQUIRE(turns.has_value());
    REQUIRE(turns->size() == 1);
  });
}

TEST_CASE("AgentSession streams deltas to an injected event sink", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injected-sink"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {provider::TextDelta{.text = "Hel"},
                   provider::TextDelta{.text = "lo"},
                   provider::StreamEnd{.stop_reason = core::StopReason::end_turn,
                                       .usage = std::nullopt,
                                       .model_used = std::nullopt}},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    CapturingEventSink sink;
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.event_sink = &sink;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto request = agent::PromptRequest{.prompt = "hello"};
    auto result = co_await (*runner)->run_prompt(std::move(request));

    REQUIRE(result.has_value());
    REQUIRE(sink.text_deltas == 2);
    REQUIRE(sink.text == "Hello");
    REQUIRE(sink.done_calls == 1);
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("AgentSession persists successful turns through the session store",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-session-persist"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    REQUIRE(assembly.session_store() != nullptr);

    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{
        provider::ScriptedTurn{
            .response = text_response("stored"),
            .deltas = {},
            .error = std::nullopt,
            .latency = {},
        },
    }};

    auto options = base_runner_options(io, assembly, cfg, fake);
    core::TurnId session_id{};
    session_id.back() = std::byte{0x42};
    options.session_id = session_id;

    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    orangutan::agent::PromptRequest prompt{.prompt = "remember"};
    prompt.images.push_back(core::ImageContent{"image/png", "aW1hZ2U="});
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "stored");

    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].role == core::Role::user);
    REQUIRE((*loaded)[0].blocks ==
            std::vector<core::Content>{core::TextContent{"remember"}, core::ImageContent{"image/png", "aW1hZ2U="}});
    REQUIRE((*loaded)[1].role == core::Role::assistant);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("stored").blocks);
  });
}

TEST_CASE("AgentSession reloads persisted history for a new runner instance",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-session-reload"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);

    core::TurnId session_id{};
    session_id[0] = std::byte{0x12};
    session_id[15] = std::byte{0x34};
    const core::ImageContent original_image{"image/png", std::string(600 * 1024, 'A')};

    {
      provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{
          provider::ScriptedTurn{
              .response = text_response("first answer"),
              .deltas = {},
              .error = std::nullopt,
              .latency = {},
          },
      }};
      auto options = base_runner_options(io, assembly, cfg, fake);
      options.session_id = session_id;
      auto runner = bootstrap::AgentSession::create(std::move(options));
      REQUIRE(runner.has_value());
      orangutan::agent::PromptRequest prompt{.prompt = "first prompt"};
      prompt.images.push_back(original_image);
      auto first = co_await (*runner)->run_prompt(std::move(prompt));
      REQUIRE(first.has_value());
    }

    RecordingProvider recording{{text_response("second answer")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.session_id = session_id;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto second = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "second prompt"});

    REQUIRE(second.has_value());
    REQUIRE(second->text == "second answer");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].messages.size() == 3);
    REQUIRE(requests[0].messages[0].role == core::Role::user);
    REQUIRE(requests[0].messages[0].blocks ==
            std::vector<core::Content>{core::TextContent{"first prompt"}, original_image});
    REQUIRE(requests[0].messages[1].role == core::Role::assistant);
    REQUIRE(requests[0].messages[1].blocks == core::Message::assistant_text("first answer").blocks);
    REQUIRE(requests[0].messages[2].role == core::Role::user);
    REQUIRE(requests[0].messages[2].blocks == core::Message::user_text("second prompt").blocks);

    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "12000000000000000000000000000034"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 4);
    REQUIRE((*loaded)[3].blocks == core::Message::assistant_text("second answer").blocks);
  });
}

TEST_CASE("AgentSession rejects oversized images before effects and accepts a subsequent image-only prompt",
          "[unit][bootstrap][prompt_runner][image]") {
  TempDir temp{"oran-bootstrap-image-input"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true, false);
    RecordingProvider fake{std::vector<provider::Response>{text_response("image received")}};
    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner);
    agent::PromptRequest oversized;
    oversized.images = {{"image/png", std::string(8 * 1024 * 1024, 'A')},
                        {"image/png", std::string(8 * 1024 * 1024, 'A')}};
    auto rejected = co_await (*runner)->run_prompt(std::move(oversized));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().kind() == core::ErrorKind::invalid_argument);
    CHECK(fake.requests().empty());

    agent::PromptRequest prompt;
    const core::ImageContent image{"image/png", "aW1hZ2U="};
    prompt.images.push_back(image);
    auto accepted = co_await (*runner)->run_prompt(std::move(prompt));
    REQUIRE(accepted);
    REQUIRE(fake.requests().size() == 1);
    REQUIRE(fake.requests().front().messages.size() == 1);
    CHECK(fake.requests().front().messages.front().blocks == std::vector<core::Content>{image});
  });
}

TEST_CASE("AgentSession uses provider execution retry before returning text",
          "[unit][bootstrap][prompt_runner][provider]") {
  TempDir temp{"oran-bootstrap-prompt-runner-retry"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("transient"),
        .latency = {},
    });
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("retried"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    auto options = base_runner_options(io, assembly, cfg, fake);
    options.retry.max_attempts = 2;

    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto prompt = orangutan::agent::PromptRequest{.prompt = "retry"};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "retried");
    REQUIRE(fake.turns_consumed() == 2);
  });
}

TEST_CASE("AgentSession publishes provider hooks through RuntimeAssembly",
          "[unit][bootstrap][prompt_runner][provider][hooks]") {
  TempDir temp{"oran-bootstrap-prompt-runner-provider-hooks"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::provider_request, hook::Event::provider_response});

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = text_response("hooked"),
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};

    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(runner.has_value());
    auto prompt = orangutan::agent::PromptRequest{.prompt = "hooks"};
    auto result = co_await (*runner)->run_prompt(std::move(prompt));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "hooked");
    REQUIRE(captures.size() == 2);
    REQUIRE(captures[0].event == hook::Event::provider_request);
    const auto* request = std::get_if<hook::ProviderRequestPayload>(&captures[0].payload);
    REQUIRE(request != nullptr);
    REQUIRE(request->who.scope_key == "scope-A");
    REQUIRE(request->who.agent_key == "coder");
    REQUIRE(request->who.identity == "operator-1");
    REQUIRE(request->origin == "test");
    REQUIRE(request->route_profile == "fake");
    REQUIRE(request->route_model == "fake-1");
    REQUIRE(request->fallback_count == 0);
    REQUIRE(request->message_count == 1);

    REQUIRE(captures[1].event == hook::Event::provider_response);
    const auto* response = std::get_if<hook::ProviderResponsePayload>(&captures[1].payload);
    REQUIRE(response != nullptr);
    REQUIRE(response->who.agent_key == "coder");
    REQUIRE(response->served_profile == "fake");
    REQUIRE(response->served_model == "fake-1");
    REQUIRE(response->served_protocol == "anthropic_messages");
    REQUIRE(response->usage.input_tokens == 3);
    REQUIRE(response->usage.output_tokens == 2);
    REQUIRE(response->stop_reason == "end_turn");
  });
}

TEST_CASE("AgentSession exposes custom tools directly and refreshes them at the next prompt",
          "[integration][bootstrap][catalog]") {
  TempDir temp{"oran-native-custom-tools"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(R"({"permissions":{"allow":[{"tool_pattern":"CustomTool"}]}})");
    auto assembly = build_assembly(temp.path(), io, false, false, false);
    tool::Registry registry;
    std::size_t calls = 0;
    REQUIRE(registry.add(
        core::ToolDef::with_no_input("CustomTool", "Read host context"),
        [&calls](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
          ++calls;
          co_return tool::Output::text_only("host context");
        }));
    agent::ToolScheduler scheduler{io.get_executor(), registry};
    RecordingProvider recording{
        {tool_response("CustomTool", "custom-1", "{}"), text_response("done"), text_response("continued")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.registry = &registry;
    options.scheduler = &scheduler;
    auto session = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "Use the host context."});
    REQUIRE(result.has_value());
    REQUIRE(calls == 1);
    auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].tools == registry.catalog());
    REQUIRE(requests[1].tools == requests[0].tools);
    REQUIRE(requests[0].system_prompt == requests[1].system_prompt);
    REQUIRE(requests[0].cache == requests[1].cache);
    REQUIRE(tool_result_output_in(requests[1], "custom-1") == "host context");

    REQUIRE(registry.add(core::ToolDef::with_no_input("AddedTool", "New host service"),
                         [](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
                           co_return tool::Output::text_only("new context");
                         }));
    result = co_await (*session)->run_prompt({.prompt = "Continue."});
    REQUIRE(result.has_value());
    requests = recording.requests();
    REQUIRE(requests.size() == 3);
    REQUIRE(requests[2].tools.size() == 2);
    REQUIRE(requests[2].tools[0].name == "AddedTool");
    REQUIRE(requests[2].tools[1].name == "CustomTool");
    REQUIRE(requests[2].system_prompt == requests[0].system_prompt);
    REQUIRE(requests[2].cache.has_value());
    REQUIRE(requests[0].cache.has_value());
    REQUIRE(requests[2].cache->prefix_hash != requests[0].cache->prefix_hash);
  });
}

TEST_CASE("AgentSession preserves explicit tool subsets and empty selections", "[integration][bootstrap][catalog]") {
  bool empty = false;
  SECTION("explicit subset") {}
  SECTION("explicit empty list") {
    empty = true;
  }
  TempDir temp{"oran-native-selected-tools"};
  test::run_async([&temp, empty](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(empty ? R"({"runtime":{"prompt":{"active_tools":[]}}})"
                                  : R"({"runtime":{"prompt":{"active_tools":["FileRead"]}}})");
    auto assembly = build_assembly(temp.path(), io, false, false, false);
    RecordingProvider recording{{text_response("done")}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(session.has_value());
    const auto result = co_await (*session)->run_prompt({.prompt = "Hello"});
    REQUIRE(result.has_value());
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].tools.size() == (empty ? 0 : 1));
    if (!empty) {
      REQUIRE(requests[0].tools[0].name == "FileRead");
    }
    REQUIRE_FALSE(requests[0].system_prompt->contains("FileWrite"));
  });
}

TEST_CASE("AgentSession renders memory framing once per prompt before loop iterations",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-framing"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    write_file(temp.path() / "note.txt", "memory framing fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 1,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.memory_framing = "memory: stable";
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("memory: stable"));
    REQUIRE(requests[1].system_prompt->contains("memory: stable"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession exposes memory orientation by default without a recall hint",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-default-off"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(
        make_longterm_record("lt-default", "Prefer small changes with clear reasons."));
    REQUIRE(upserted.has_value());

    RecordingProvider recording{{text_response("done")}};
    auto runner = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(runner.has_value());

    auto result =
        co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "Help me review this change."});

    REQUIRE(result.has_value());
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Memory index:"));
    REQUIRE(requests[0].system_prompt->contains("lt-default"));
    REQUIRE(requests[0].system_prompt->contains("Prefer small changes with clear reasons."));
  });
}

TEST_CASE("AgentSession recalls long-term memory once before loop iterations",
          "[integration][bootstrap][prompt_runner][memory][core_boundary]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-recall"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(
        make_longterm_record("lt-recall-1", "Recall plumbing reaches the prompt boundary."));
    REQUIRE(upserted.has_value());
    auto foreign = make_longterm_record("foreign", "Recall plumbing belongs to another workspace.");
    foreign.key.scope_key = "scope-B";
    auto foreign_saved = co_await assembly.longterm_memory_backend()->upsert(std::move(foreign));
    REQUIRE(foreign_saved.has_value());
    std::vector<MemoryHookCapture> hook_captures;
    auto sink = memory_capture_sink(hook_captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_read_after});
    write_file(temp.path() / "note.txt", "long-term recall fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = true, .limit = 5};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "recall plumbing"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Memory index:"));
    REQUIRE(requests[0].system_prompt->contains("Recall plumbing reaches the prompt boundary."));
    REQUIRE_FALSE(requests[0].system_prompt->contains("another workspace"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_read_after);
    const auto* read = std::get_if<hook::MemoryReadPayload>(&hook_captures[0].payload);
    REQUIRE(read != nullptr);
    REQUIRE(read->who.scope_key == "scope-A");
    REQUIRE(read->who.agent_key == "coder");
    REQUIRE(read->who.identity == "operator-1");
    REQUIRE(read->source == "MemoryRecall:index");
    REQUIRE(read->query.empty());
    REQUIRE(read->redacted_query_bytes == 0);
    REQUIRE(read->limit == 5);
    REQUIRE(read->kinds.empty());
    REQUIRE(read->match_count == 1);
    REQUIRE(read->hits.size() == 1);
    REQUIRE(read->hits[0].record.id == "lt-recall-1");
    REQUIRE(read->hits[0].record.body == "Recall plumbing reaches the prompt boundary.");
    REQUIRE(read->hits[0].record.tags.empty());
    REQUIRE(read->hits[0].redacted_record.has_value());
    REQUIRE(read->hits[0].redacted_record->body_bytes ==
            std::string_view{"Recall plumbing reaches the prompt boundary."}.size());
    REQUIRE(read->finished_at >= read->started_at);
  });
}

TEST_CASE("AgentSession dispatches MemoryRecall through long-term runtime",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-recall-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(
        make_longterm_record("lt-tool-recall", "Memory tool found toolrecallanchor in the project."));
    REQUIRE(upserted.has_value());
    std::vector<MemoryHookCapture> hook_captures;
    auto sink = memory_capture_sink(hook_captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_read_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-1",
                .name = "MemoryRecall",
                .input_json = R"({"query":"toolrecallanchor","limit":5,"kinds":["project"]})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = false};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "use memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-1");
    REQUIRE(output.contains("MemoryRecall: 1 match"));
    REQUIRE(output.contains("Memory tool found toolrecallanchor in the project."));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_recall")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-recall")"));
    REQUIRE(data_json->contains(R"("match_count":1)"));

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_read_after);
    const auto* read = std::get_if<hook::MemoryReadPayload>(&hook_captures[0].payload);
    REQUIRE(read != nullptr);
    REQUIRE(read->who.scope_key == "scope-A");
    REQUIRE(read->who.agent_key == "coder");
    REQUIRE(read->who.identity == "operator-1");
    REQUIRE(read->source == "MemoryRecall");
    REQUIRE(read->query == "toolrecallanchor");
    REQUIRE(read->redacted_query_bytes == std::string_view{"toolrecallanchor"}.size());
    REQUIRE(read->limit == 5);
    REQUIRE(read->kinds == std::vector<std::string>{"project"});
    REQUIRE(read->match_count == 1);
    REQUIRE(read->hits.size() == 1);
    REQUIRE(read->hits[0].record.id == "lt-tool-recall");
    REQUIRE(read->hits[0].record.body == "Memory tool found toolrecallanchor in the project.");
    REQUIRE(read->hits[0].record.tags == std::vector<std::string>{"recall"});
    REQUIRE(read->hits[0].redacted_record.has_value());
    REQUIRE(read->hits[0].redacted_record->body_bytes ==
            std::string_view{"Memory tool found toolrecallanchor in the project."}.size());
    REQUIRE(read->finished_at >= read->started_at);
  });
}

TEST_CASE("AgentSession dispatches MemoryRemember through long-term backend",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-remember-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    auto sink = memory_capture_sink(hook_captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_write_before, hook::Event::memory_write_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-write-1",
                .name = "MemoryRemember",
                .input_json =
                    R"({"id":"lt-tool-remember","kind":"project","title":"Remembered note","body":"Memory remember wrote rememberanchor into the project.","importance":0.75,"tags":["remember","tool"],"linked_record_ids":["lt-tool-recall"]})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "write memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-write-1");
    REQUIRE(output.contains("MemoryRemember: saved project record lt-tool-remember"));
    REQUIRE(output.contains("title: Remembered note"));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-write-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_remember")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-remember")"));
    REQUIRE(data_json->contains(R"("scope_key":"scope-A")"));
    REQUIRE(data_json->contains(R"("tags":["remember","tool"])"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-remember",
        .scope_key = "scope-A",
    });
    REQUIRE(stored.has_value());
    REQUIRE(stored->kind == memory::longterm::RecordKind::project);
    REQUIRE(stored->title == "Remembered note");
    REQUIRE(stored->body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(stored->importance == 0.75);
    REQUIRE(stored->tags == std::vector<std::string>{"remember", "tool"});
    REQUIRE(stored->linked_record_ids == std::vector<std::string>{"lt-tool-recall"});

    REQUIRE(hook_captures.size() == 2);
    REQUIRE(hook_captures[0].event == hook::Event::memory_write_before);
    const auto* before = std::get_if<hook::MemoryWritePayload>(&hook_captures[0].payload);
    REQUIRE(before != nullptr);
    REQUIRE(before->who.scope_key == "scope-A");
    REQUIRE(before->who.agent_key == "coder");
    REQUIRE(before->who.identity == "operator-1");
    REQUIRE(before->record.id == "lt-tool-remember");
    REQUIRE(before->record.scope_key == "scope-A");
    REQUIRE(before->record.kind == "project");
    REQUIRE(before->record.title == "Remembered note");
    REQUIRE(before->record.body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(before->record.tags == std::vector<std::string>{"remember", "tool"});
    REQUIRE(before->redacted_record.has_value());
    REQUIRE(before->redacted_record->body_bytes ==
            std::string_view{"Memory remember wrote rememberanchor into the project."}.size());

    REQUIRE(hook_captures[1].event == hook::Event::memory_write_after);
    const auto* after = std::get_if<hook::MemoryWritePayload>(&hook_captures[1].payload);
    REQUIRE(after != nullptr);
    REQUIRE(after->record.id == "lt-tool-remember");
    REQUIRE(after->record.body == "Memory remember wrote rememberanchor into the project.");
    REQUIRE(after->finished_at >= after->started_at);
  });
}

TEST_CASE("AgentSession lets MemoryWrite.before veto MemoryRemember",
          "[unit][bootstrap][prompt_runner][memory][hook]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-write-veto"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    auto veto = hook::HookDecision{};
    veto.kind = hook::HookDecisionKind::veto;
    veto.reason = "memory policy";
    auto sink = memory_capture_sink(hook_captures, std::move(veto));
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_write_before, hook::Event::memory_write_after});

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-write-veto-1",
                .name = "MemoryRemember",
                .input_json =
                    R"({"id":"lt-tool-remember-veto","kind":"project","title":"Blocked note","body":"This should not persist."})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "write blocked memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-write-veto-1");
    REQUIRE(output.contains("memory write blocked by hook"));
    REQUIRE(output.contains("memory policy"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-remember-veto",
        .scope_key = "scope-A",
    });
    REQUIRE_FALSE(stored.has_value());
    REQUIRE(stored.error().kind() == core::ErrorKind::not_found);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_write_before);
    const auto* before = std::get_if<hook::MemoryWritePayload>(&hook_captures[0].payload);
    REQUIRE(before != nullptr);
    REQUIRE(before->record.id == "lt-tool-remember-veto");
    REQUIRE(before->record.body == "This should not persist.");
  });
}

TEST_CASE("AgentSession dispatches MemoryForget through long-term backend",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-memory-forget-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    REQUIRE(assembly.longterm_memory_backend() != nullptr);
    std::vector<MemoryHookCapture> hook_captures;
    auto sink = memory_capture_sink(hook_captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_forget});
    auto upserted = co_await assembly.longterm_memory_backend()->upsert(
        make_longterm_record("lt-tool-forget", "Memory forget should remove forgetanchor."));
    REQUIRE(upserted.has_value());

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "memory-forget-1",
                .name = "MemoryForget",
                .input_json = R"({"id":"lt-tool-forget"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "forget memory"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    const auto output = tool_result_output_in(requests[1], "memory-forget-1");
    REQUIRE(std::ranges::contains(requests[0].tools, std::string_view{"MemoryForget"}, &core::ToolDef::name));
    REQUIRE(output.contains("MemoryForget: removed record lt-tool-forget"));
    const auto data_json = tool_result_data_json_in(requests[1], "memory-forget-1");
    REQUIRE(data_json.has_value());
    REQUIRE(data_json->contains(R"("kind":"memory_forget")"));
    REQUIRE(data_json->contains(R"("id":"lt-tool-forget")"));
    REQUIRE(data_json->contains(R"("scope_key":"scope-A")"));

    auto stored = co_await assembly.longterm_memory_backend()->get(memory::longterm::RecordKey{
        .id = "lt-tool-forget",
        .scope_key = "scope-A",
    });
    REQUIRE_FALSE(stored.has_value());
    REQUIRE(stored.error().kind() == core::ErrorKind::not_found);

    REQUIRE(hook_captures.size() == 1);
    REQUIRE(hook_captures[0].event == hook::Event::memory_forget);
    const auto* forget = std::get_if<hook::MemoryForgetPayload>(&hook_captures[0].payload);
    REQUIRE(forget != nullptr);
    REQUIRE(forget->who.scope_key == "scope-A");
    REQUIRE(forget->who.agent_key == "coder");
    REQUIRE(forget->who.identity == "operator-1");
    REQUIRE(forget->id == "lt-tool-forget");
    REQUIRE(forget->scope_key == "scope-A");
    REQUIRE(forget->finished_at >= forget->started_at);
  });
}

TEST_CASE("AgentSession rejects long-term recall without assembly runtime",
          "[unit][bootstrap][prompt_runner][memory]") {
  TempDir temp{"oran-bootstrap-prompt-runner-longterm-no-runtime"};
  asio::io_context io;
  auto cfg = config::Config{};
  auto assembly = build_assembly(temp.path(), io, false, false, false);
  RecordingProvider recording{{text_response("unused")}};

  auto options = base_runner_options(io, assembly, cfg, recording);
  options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = true, .limit = 5};
  auto runner = bootstrap::AgentSession::create(std::move(options));

  REQUIRE_FALSE(runner.has_value());
  REQUIRE(runner.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("AgentSession renders default system preamble once per prompt before loop iterations",
          "[unit][bootstrap][prompt_runner][prompt]") {
  TempDir temp{"oran-bootstrap-prompt-runner-system-preamble"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    write_file(temp.path() / "note.txt", "system preamble fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("You are Orangutan"));
    REQUIRE(requests[0].system_prompt->contains("Operating principles:"));
    REQUIRE_FALSE(requests[0].system_prompt->contains("Tool: FileRead"));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession preserves host-supplied instructions across iterations", "[unit][bootstrap][prompt_runner]") {
  TempDir temp{"oran-bootstrap-prompt-runner-agent-overlay"};
  auto cfg = config::Config{};

  test::run_async([&temp, &cfg](asio::io_context& io) -> async::Awaitable<void> {
    auto assembly = build_assembly(temp.path(), io, false);
    write_file(temp.path() / "note.txt", "agent overlay fixture\n");

    RecordingProvider recording{{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "read-1",
                .name = "FileRead",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("done"),
    }};

    auto options = base_runner_options(io, assembly, cfg, recording);
    options.per_agent_overlay = "Agent overlay: prefer concise, source-backed answers.";
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());

    auto result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read"});

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    const auto requests = recording.requests();
    REQUIRE(requests.size() == 2);
    REQUIRE(requests[0].system_prompt.has_value());
    REQUIRE(requests[1].system_prompt.has_value());
    REQUIRE(requests[0].system_prompt->contains("Agent overlay: prefer concise, source-backed answers."));
    REQUIRE(*requests[0].system_prompt == *requests[1].system_prompt);
  });
}

TEST_CASE("AgentSession shares path exclusion through an injected scheduler",
          "[unit][bootstrap][prompt_runner][scheduler]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injected-scheduler"};
  test::run_async(
      [&temp](asio::io_context& io) -> async::Awaitable<void> {
        using namespace asio::experimental::awaitable_operators;
        auto cfg = parse_config(R"({
          "runtime": {"prompt": {"active_tools": ["PathProbe"]}},
          "permissions": {"allow": [{"tool_pattern": "PathProbe"}]}
        })");
        auto assembly = build_assembly(temp.path(), io, false, false, false);
        async::Channel<int> first_entered{io.get_executor(), 1};
        async::Channel<int> second_entered{io.get_executor(), 1};
        async::Channel<int> release_first{io.get_executor(), 1};
        bool first_active = false;
        bool overlap = false;
        tool::Registry registry;
        auto definition = core::ToolDef{
            .name = "PathProbe",
            .description = "Hold a path across sessions",
            .input_schema_json = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})",
            .required_capabilities = {core::Capability::write_file},
        };
        REQUIRE(registry.add(std::move(definition),
                             [&first_entered, &second_entered, &release_first, &first_active, &overlap](
                                 std::string_view,
                                 tool::DispatchContext& context) -> async::Awaitable<core::Result<tool::Output>> {
                               const bool first = context.agent_key == "first";
                               overlap = overlap || (!first && first_active);
                               if (first)
                                 first_active = true;
                               auto entered = (first ? first_entered : second_entered).try_send(0);
                               if (!entered)
                                 co_return std::unexpected(std::move(entered).error());
                               if (first) {
                                 auto released = co_await release_first.receive();
                                 first_active = false;
                                 if (!released)
                                   co_return std::unexpected(std::move(released).error());
                               }
                               co_return tool::Output::text_only("path released");
                             }));
        agent::ToolScheduler scheduler{io.get_executor(), registry};
        const auto call = provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "write-1",
                .name = "PathProbe",
                .input_json = R"({"path":"note.txt"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .model_used = std::string{"fake-1"},
        };
        RecordingProvider first_provider{{call, text_response("first done")}};
        RecordingProvider second_provider{{call, text_response("second done")}};
        auto options = base_runner_options(io, assembly, cfg, first_provider);
        options.agent_key = "first";
        options.registry = &registry;
        options.scheduler = &scheduler;
        auto first = bootstrap::AgentSession::create(options);
        REQUIRE(first.has_value());
        options.agent_key = "second";
        options.provider = &second_provider;
        auto second = bootstrap::AgentSession::create(std::move(options));
        REQUIRE(second.has_value());

        bool entered_before_release = false;
        bool released = false;
        auto observe_then_release =
            [&second_entered, &io, &entered_before_release, &released, &release_first]() -> async::Awaitable<void> {
          auto early = co_await (second_entered.receive() ||
                                 async::sleep_for(io.get_executor(), std::chrono::milliseconds{100}));
          entered_before_release = std::holds_alternative<core::Result<int>>(early);
          released = release_first.try_send(0).has_value();
        };
        auto run_second =
            [&first_entered, &second, &observe_then_release]() -> async::Awaitable<core::Result<agent::PromptResult>> {
          auto entered = co_await first_entered.receive();
          if (!entered)
            co_return std::unexpected(std::move(entered).error());
          co_return co_await ((*second)->run_prompt({.prompt = "write"}) && observe_then_release());
        };
        auto [first_result, second_result] = co_await ((*first)->run_prompt({.prompt = "write"}) && run_second());
        REQUIRE(first_result.has_value());
        REQUIRE(second_result.has_value());
        REQUIRE(first_result->text == "first done");
        REQUIRE(second_result->text == "second done");
        REQUIRE(released);
        REQUIRE_FALSE(entered_before_release);
        REQUIRE_FALSE(overlap);
      },
      std::chrono::seconds{3});
}

TEST_CASE("AgentSession::create rejects a registry/scheduler supplied without its pair",
          "[unit][bootstrap][prompt_runner][scheduler]") {
  TempDir temp{"oran-bootstrap-prompt-runner-injection-validate"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    provider::FakeProvider fake{std::vector<provider::ScriptedTurn>{}};

    tool::Registry registry;
    REQUIRE(tool::register_builtins(registry).has_value());
    agent::ToolScheduler scheduler{io.get_executor(), registry};

    // A scheduler without its registry (and vice versa) is rejected up front:
    // the runner's `.tools` and the scheduler's registry must be the same
    // instance, so the pair is all-or-nothing.
    auto only_scheduler = base_runner_options(io, assembly, cfg, fake);
    only_scheduler.scheduler = &scheduler;
    REQUIRE_FALSE(bootstrap::AgentSession::create(std::move(only_scheduler)).has_value());

    auto only_registry = base_runner_options(io, assembly, cfg, fake);
    only_registry.registry = &registry;
    REQUIRE_FALSE(bootstrap::AgentSession::create(std::move(only_registry)).has_value());

    co_return;
  });
}

// The session and scheduler share a strand while multiple IO workers drive
// the runtime. Concurrent tool calls must preserve that coordinating boundary.
TEST_CASE("AgentSession multi-tool batches complete on a multi-worker runtime",
          "[unit][bootstrap][prompt_runner][scheduler][concurrency]") {
  TempDir temp{"oran-bootstrap-prompt-runner-multiworker"};
  write_file(temp.path() / "left.txt", "left fixture\n");
  write_file(temp.path() / "right.txt", "right fixture\n");

  auto runtime = async::Runtime{async::RuntimeConfig{.io_workers = 4, .cpu_workers = 1}};
  auto agent_strand = runtime.make_strand();

  auto cfg = config::Config{};
  auto assembly = build_assembly(temp.path(), runtime.executor(), false);

  // Each scripted turn fans two parallel FileRead calls through the
  // scheduler before the terminal text turn ends the loop (16-iteration cap).
  constexpr int kToolTurns = 8;
  std::vector<provider::ScriptedTurn> plan;
  for (int turn = 0; turn < kToolTurns; ++turn) {
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "read-left-" + std::to_string(turn),
                                                .name = "FileRead",
                                                .input_json = R"({"path":"left.txt"})"},
                           core::ToolUseContent{.id = "read-right-" + std::to_string(turn),
                                                .name = "FileRead",
                                                .input_json = R"({"path":"right.txt"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = provider::Usage{.input_tokens = 1,
                                         .output_tokens = 1,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = std::chrono::milliseconds{1},
    });
  }
  plan.push_back(provider::ScriptedTurn{
      .response = text_response("multiworker done"),
      .deltas = {},
      .error = std::nullopt,
      .latency = {},
  });
  provider::FakeProvider fake{std::move(plan)};

  auto runner = bootstrap::AgentSession::create(base_runner_options(agent_strand, assembly, cfg, fake));
  REQUIRE(runner.has_value());

  // Watchdog so a lost completion fails loudly instead of hanging the bucket.
  bool timed_out = false;
  asio::steady_timer watchdog{runtime.executor()};
  watchdog.expires_after(std::chrono::seconds{20});
  watchdog.async_wait([&](const asio::error_code& ec) {
    if (!ec) {
      timed_out = true;
      runtime.stop();
    }
  });

  auto result = std::optional<core::Result<orangutan::agent::PromptResult>>{};
  asio::co_spawn(
      agent_strand,
      [&]() -> async::Awaitable<void> {
        result = co_await (*runner)->run_prompt(orangutan::agent::PromptRequest{.prompt = "read both"});
        watchdog.cancel();
        runtime.stop();
        co_return;
      },
      asio::detached);

  REQUIRE(runtime.run().has_value());
  REQUIRE_FALSE(timed_out);
  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  REQUIRE((*result)->text == "multiworker done");
  REQUIRE(fake.turns_consumed() == kToolTurns + 1);
}

TEST_CASE("automatic orientation uses scoped memory without generic tool permission",
          "[integration][bootstrap][memory][core_boundary]") {
  TempDir temp{"oran-recall-policy"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(R"({"permissions":{"deny":[{"tool_pattern":"MemoryRecall"}]}})");
    auto assembly = build_assembly(temp.path(), io, false);
    auto saved =
        co_await assembly.longterm_memory_backend()->upsert(make_longterm_record("private", "Private recall content"));
    REQUIRE(saved.has_value());
    std::vector<MemoryHookCapture> captures;
    auto sink = memory_capture_sink(captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_read_after});
    RecordingProvider recording{{text_response("unexpected")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.longterm_recall = bootstrap::LongtermRecallOptions{.enabled = true};
    auto session = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "Private recall"});

    REQUIRE(result.has_value());
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().system_prompt.has_value());
    CHECK_FALSE(requests.front().system_prompt->contains("Memory index unavailable"));
    CHECK(requests.front().system_prompt->contains("Private recall content"));
    CHECK_FALSE(requests.front().system_prompt->contains("No saved notes match this index"));
    CHECK_FALSE(captures.empty());
  });
}

TEST_CASE("memory corrections become visible and readable in a fresh session",
          "[integration][bootstrap][memory][core_boundary][preservation]") {
  TempDir temp{"oran-memory-learning-loop"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(R"({"permissions":{"allow":[{"tool_pattern":"MemoryRemember"}]}})");
    auto original = make_longterm_record("response-style", "Use English for explanations.\n");
    original.kind = memory::longterm::RecordKind::feedback;
    original.title = "Response style";
    original.body += std::string(1000, 'x') + "ORIGINAL_FULL_DETAIL";
    {
      auto assembly = build_assembly(temp.path(), io, false);
      auto seeded = co_await assembly.longterm_memory_backend()->upsert(original);
      REQUIRE(seeded.has_value());
      RecordingProvider provider{{
          tool_response("MemoryRecall", "read-old", R"({"id":"response-style"})"),
          tool_response(
              "MemoryRemember",
              "correct",
              R"({"id":"response-style","kind":"feedback","title":"Response style","body":"用中文给出简洁结论。需要时再解释理由。"})"),
          text_response("我会先给出简洁的中文结论。"),
      }};
      auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, provider));
      REQUIRE(session.has_value());
      auto first = co_await (*session)->run_prompt({.prompt = "以后用中文给我结论，理由放到必要的时候再说。"});
      REQUIRE(first.has_value());
      const auto requests = provider.requests();
      REQUIRE(requests.size() == 3);
      REQUIRE(requests[0].system_prompt.has_value());
      CHECK(requests[0].system_prompt->contains("response-style"));
      CHECK_FALSE(requests[0].system_prompt->contains("ORIGINAL_FULL_DETAIL"));
      CHECK(requests[0].system_prompt == requests[1].system_prompt);
      CHECK(requests[1].system_prompt == requests[2].system_prompt);
      CHECK(tool_result_output_in(requests[1], "read-old").contains("ORIGINAL_FULL_DETAIL"));
      CHECK(tool_result_output_in(requests[2], "correct").contains("saved feedback record response-style"));
      auto saved = co_await assembly.longterm_memory_backend()->get(original.key);
      REQUIRE(saved.has_value());
      CHECK(saved->created_at == original.created_at);
      CHECK(saved->body == "用中文给出简洁结论。需要时再解释理由。");
    }
    {
      auto reopened = build_assembly(temp.path(), io, false);
      RecordingProvider provider{{
          tool_response("MemoryRecall", "apply", R"({"id":"response-style"})"),
          text_response("这个改动可行。"),
      }};
      auto session = bootstrap::AgentSession::create(base_runner_options(io, reopened, cfg, provider));
      REQUIRE(session.has_value());
      auto next = co_await (*session)->run_prompt({.prompt = "帮我评估这个改动。"});
      REQUIRE(next.has_value());
      const auto requests = provider.requests();
      REQUIRE(requests.size() == 2);
      REQUIRE(requests.front().system_prompt.has_value());
      CHECK(requests.front().system_prompt->contains("用中文给出简洁结论"));
      CHECK_FALSE(requests.front().system_prompt->contains("Use English for explanations"));
      CHECK(tool_result_output_in(requests[1], "apply").contains("需要时再解释理由"));
      auto index = co_await memory::longterm::index(*reopened.longterm_memory_backend(), {.scope_key = "scope-A"});
      REQUIRE(index.has_value());
      REQUIRE(index->entries.size() == 1);
      CHECK(index->entries.front().key.id == "response-style");
    }
  });
}

TEST_CASE("session memory orientation consumes configuration and honors opt-out",
          "[integration][bootstrap][memory][core_boundary]") {
  TempDir temp{"oran-memory-config"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = parse_config(R"({"memory":{"longterm":{"recall":{"enabled":false}}}})");
    auto assembly = build_assembly(temp.path(), io, false);
    auto seeded = co_await assembly.longterm_memory_backend()->upsert(make_longterm_record("stored", "Saved context."));
    REQUIRE(seeded.has_value());
    std::vector<MemoryHookCapture> captures;
    auto sink = memory_capture_sink(captures);
    assembly.hook_bus().subscribe(sink, {hook::Event::memory_read_after});
    RecordingProvider provider{{text_response("done")}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, provider));
    REQUIRE(session.has_value());
    auto result = co_await (*session)->run_prompt({.prompt = "Hello"});
    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 1);
    CHECK_FALSE(provider.requests().front().system_prompt->contains("Memory index:"));
    CHECK(captures.empty());
  });
}

TEST_CASE("an automatic memory index cannot be rewritten into full prompt content",
          "[integration][bootstrap][memory][hook]") {
  TempDir temp{"oran-memory-index-rewrite"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false);
    auto seeded = co_await assembly.longterm_memory_backend()->upsert(
        make_longterm_record("note", "DETAIL_MUST_STAY_OUT_OF_AUTOMATIC_PREFIX"));
    REQUIRE(seeded.has_value());
    hook::Sink rewrite{
        .id = "rewrite-index",
        .decide = [](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
          auto decision = hook::HookDecision{};
          if (std::get<hook::ToolBeforePayload>(*payload).tool_name == "MemoryRecall") {
            decision.kind = hook::HookDecisionKind::rewrite;
            decision.rewritten_input_json = R"({"id":"note"})";
          }
          co_return decision;
        },
    };
    assembly.hook_bus().subscribe(rewrite, {hook::Event::tool_before});
    RecordingProvider provider{{text_response("done")}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, provider));
    REQUIRE(session.has_value());
    auto result = co_await (*session)->run_prompt({.prompt = "Hello"});
    REQUIRE(result.has_value());
    const auto requests = provider.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().system_prompt.has_value());
    CHECK(requests.front().system_prompt->contains("Memory index unavailable"));
    CHECK_FALSE(requests.front().system_prompt->contains("DETAIL_MUST_STAY_OUT_OF_AUTOMATIC_PREFIX"));
  });
}

TEST_CASE("memory writes run without approval only when enabled for the turn",
          "[integration][bootstrap][memory][core_boundary]") {
  bool advertised = true;
  SECTION("default tool exposure") {}
  SECTION("model names an unadvertised tool") {
    advertised = false;
  }
  TempDir temp{"oran-memory-write-permission"};
  test::run_async([&temp, advertised](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = advertised ? config::Config{} : parse_config(R"({"runtime":{"prompt":{"active_tools":[]}}})");
    auto assembly = build_assembly(temp.path(), io, false);
    RecordingProvider provider{{
        tool_response("MemoryRemember",
                      "save",
                      R"({"id":"style","kind":"user","title":"Response style","body":"Prefer concise replies."})"),
        text_response("Memory operation handled."),
    }};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, provider));
    REQUIRE(session.has_value());
    auto result = co_await (*session)->run_prompt({.prompt = "Keep your explanations brief."});
    REQUIRE(result.has_value());
    const auto requests = provider.requests();
    REQUIRE(requests.size() == 2);
    CHECK(std::ranges::contains(requests[0].tools, std::string_view{"MemoryRemember"}, &core::ToolDef::name) ==
          advertised);
    CHECK_FALSE(tool_result_output_in(requests[1], "save").contains("approval"));
    auto stored = co_await assembly.longterm_memory_backend()->get({.id = "style", .scope_key = "scope-A"});
    REQUIRE(stored.has_value() == advertised);
    if (!advertised) {
      CHECK(tool_result_output_in(requests[1], "save").contains("not enabled"));
      CHECK(stored.error().kind() == core::ErrorKind::not_found);
    }
  });
}

TEST_CASE("a session advertises memory tools only when memory is available",
          "[integration][bootstrap][memory][core_boundary]") {
  TempDir temp{"oran-tool-services"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, false, false);
    RecordingProvider recording{{text_response("done")}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, recording));
    REQUIRE(session.has_value());

    auto result = co_await (*session)->run_prompt({.prompt = "Hello"});

    REQUIRE(result.has_value());
    CHECK(result->text == "done");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().system_prompt.has_value());
    CHECK(std::ranges::contains(requests.front().tools, std::string_view{"FileRead"}, &core::ToolDef::name));
    CHECK_FALSE(std::ranges::contains(requests.front().tools, std::string_view{"MemoryRecall"}, &core::ToolDef::name));
    CHECK_FALSE(
        std::ranges::contains(requests.front().tools, std::string_view{"MemoryRemember"}, &core::ToolDef::name));
    CHECK_FALSE(std::ranges::contains(requests.front().tools, std::string_view{"MemoryForget"}, &core::ToolDef::name));
  });
}

TEST_CASE("AgentSession does not commit incomplete responses and can continue", "[integration][bootstrap][memory]") {
  const auto reason = GENERATE(core::StopReason::max_tokens, core::StopReason::cancelled, core::StopReason::error);
  const bool with_tool = GENERATE(false, true);
  TempDir temp{"oran-session-incomplete"};
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    auto incomplete =
        with_tool ? tool_response("FileWrite", "write-1", R"({"path":"must-not-exist.txt","content":"partial"})")
                  : text_response("partial answer");
    incomplete.stop_reason = reason;
    RecordingProvider recording{{text_response("saved answer"), std::move(incomplete), text_response("continued")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    options.session_id.back() = std::byte{0x42};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto first = co_await (*runner)->run_prompt({.prompt = "saved prompt"});
    REQUIRE(first.has_value());
    auto failed = co_await (*runner)->run_prompt({.prompt = "unfinished prompt"});
    REQUIRE_FALSE(failed.has_value());
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "must-not-exist.txt"));
    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("saved answer").blocks);
    auto continued = co_await (*runner)->run_prompt({.prompt = "continue"});
    REQUIRE(continued.has_value());
    REQUIRE(continued->text == "continued");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 3);
    REQUIRE(requests.back().messages.size() == 3);
    REQUIRE(requests.back().messages[0].blocks == core::Message::user_text("saved prompt").blocks);
    REQUIRE(requests.back().messages[1].blocks == core::Message::assistant_text("saved answer").blocks);
    REQUIRE(requests.back().messages[2].blocks == core::Message::user_text("continue").blocks);
  });
}

TEST_CASE("AgentSession rejects duplicate tool IDs without file effects or transcript changes",
          "[integration][bootstrap][tool-identity]") {
  TempDir temp{"oran-session-duplicate-tool"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    auto malformed = tool_response("FileWrite", "same-id", R"({"path":"first.txt","content":"first"})");
    malformed.blocks.emplace_back(core::ToolUseContent{.id = "same-id",
                                                       .name = "FileWrite",
                                                       .input_json = R"({"path":"second.txt","content":"second"})"});
    RecordingProvider recording{{text_response("saved answer"), std::move(malformed), text_response("continued")}};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    options.session_id.back() = std::byte{0x42};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto first = co_await (*runner)->run_prompt({.prompt = "saved prompt"});
    REQUIRE(first.has_value());
    auto failed = co_await (*runner)->run_prompt({.prompt = "write both files"});
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().kind() == core::ErrorKind::upstream);
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "first.txt"));
    REQUIRE_FALSE(std::filesystem::exists(temp.path() / "second.txt"));
    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("saved answer").blocks);
    auto continued = co_await (*runner)->run_prompt({.prompt = "continue"});
    REQUIRE(continued.has_value());
    REQUIRE(continued->text == "continued");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 3);
    REQUIRE(requests.back().messages.size() == 3);
    REQUIRE(requests.back().messages[2].blocks == core::Message::user_text("continue").blocks);
  });
}

TEST_CASE("AgentSession resumes completed history after a failed transcript commit",
          "[integration][bootstrap][memory][atomic]") {
  TempDir temp{"oran-session-atomic-turn"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, true);
    auto database =
        storage::Connection::open(storage::ConnectionOptions{.path = std::string{assembly.sessions_path()}});
    REQUIRE(database.has_value());
    auto trigger = database->execute(R"sql(
      CREATE TRIGGER reject_turn BEFORE INSERT ON session_messages
      WHEN instr(NEW.content_json, 'reject-turn') > 0
      BEGIN SELECT RAISE(ABORT, 'reject completed turn'); END;
    )sql");
    REQUIRE(trigger.has_value());
    RecordingProvider recording{{
        text_response("saved answer"),
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "remember-1",
                .name = "MemoryRemember",
                .input_json = R"({"id":"accepted-note","kind":"project","title":"Note","body":"accepted effect"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
        text_response("reject-turn"),
        text_response("continued answer"),
    }};
    auto options = base_runner_options(io, assembly, cfg, recording);
    options.mode = permission::Mode::permissive;
    options.session_id.back() = std::byte{0x42};
    auto runner = bootstrap::AgentSession::create(std::move(options));
    REQUIRE(runner.has_value());
    auto first = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "saved prompt"});
    REQUIRE(first.has_value());

    auto failed = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "write a note"});

    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().kind() == core::ErrorKind::storage);
    REQUIRE(std::ranges::contains(failed.error().context(),
                                  core::Error::ContextEntry{"sqlite_message", "reject completed turn"}));
    auto loaded =
        co_await assembly.session_store()->load(memory::session::SessionId{.value = "00000000000000000000000000000042"},
                                                memory::session::AgentKey{.value = "coder"});
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    REQUIRE((*loaded)[0].blocks == core::Message::user_text("saved prompt").blocks);
    REQUIRE((*loaded)[1].blocks == core::Message::assistant_text("saved answer").blocks);
    auto note = co_await assembly.longterm_memory_backend()->get(
        memory::longterm::RecordKey{.id = "accepted-note", .scope_key = "scope-A"});
    REQUIRE(note.has_value());
    REQUIRE(note->body == "accepted effect");

    auto continued = co_await (*runner)->run_prompt(agent::PromptRequest{.prompt = "continue"});
    REQUIRE(continued.has_value());
    REQUIRE(continued->text == "continued answer");
    const auto requests = recording.requests();
    REQUIRE(requests.size() == 4);
    REQUIRE(requests.back().messages.size() == 3);
    REQUIRE(requests.back().messages[0].blocks == core::Message::user_text("saved prompt").blocks);
    REQUIRE(requests.back().messages[1].blocks == core::Message::assistant_text("saved answer").blocks);
    REQUIRE(requests.back().messages[2].blocks == core::Message::user_text("continue").blocks);
  });
}

TEST_CASE("Session compaction retains early task context across pages and reopening", "[bootstrap][context]") {
  TempDir temp{"oran-working-context"};
  test::run_async(
      [&](asio::io_context& io) -> async::Awaitable<void> {
        class ContextProvider final : public provider::System {
        public:
          mutable std::size_t summaries{};
          mutable std::size_t answers{};
          bool fail_answer{};
          async::Awaitable<core::Result<provider::Response>>
          send(provider::Request request, provider::ModelTarget, provider::EventSink*) const override {
            std::string text;
            for (const auto& message : request.messages)
              for (const auto& block : message.blocks)
                if (auto* content = std::get_if<core::TextContent>(&block))
                  text += content->text;
            REQUIRE(text.contains("Preserve dragonfruit"));
            if (request.system_prompt.value_or("").starts_with("Produce a bounded session handoff")) {
              ++summaries;
              REQUIRE(request.tools.empty());
              co_return text_response(
                  "Objective:\nPreserve dragonfruit.\nConstraints:\nNo new dependencies.\nDecisions:\nUse existing "
                  "parser [1].\nCompleted:\nRead files.\nPending:\nRun tests.\nReferences:\nsrc/parser.cpp [1]\n");
            }
            ++answers;
            if (fail_answer)
              co_return std::unexpected(core::Error::internal("controlled answer failure"));
            co_return text_response("Continue the parser task.");
          }
        } backend;
        auto cfg = config::Config{};
        auto assembly = build_assembly(temp.path(), io, false, true, false);
        auto id = core::generate_turn_id();
        REQUIRE(id);
        memory::session::SessionId session_id{core::format_turn_id_hex(*id)};
        memory::session::AgentKey agent_key{"coder"};
        auto* store = assembly.session_store();
        REQUIRE(store);
        std::vector<core::Message> history;
        for (int i = 0; i < 90; ++i) {
          history.push_back(
              core::Message::user_text(i == 0 ? "Preserve dragonfruit. No new dependencies." : std::string(300, 'u')));
          history.push_back(core::Message::assistant_text(std::string(300, 'a')));
        }
        auto seeded = co_await store->append_all(session_id, agent_key, history);
        REQUIRE(seeded);
        auto options = base_runner_options(io, assembly, cfg, backend);
        options.session_id = *id;
        options.system_preamble = "Agent";
        options.context = {.max_tokens = 16384, .summary_max_bytes = 512};
        options.max_tokens = 512;
        backend.fail_answer = true;
        {
          auto runner = bootstrap::AgentSession::create(options);
          REQUIRE(runner);
          auto failed = co_await (*runner)->run_prompt({.prompt = "Continue"});
          REQUIRE_FALSE(failed);
        }
        auto unchanged = co_await store->load_context(session_id, agent_key);
        REQUIRE(unchanged);
        REQUIRE(unchanged->message_count == 180);
        REQUIRE(unchanged->checkpoint.covered_sequence == 0);
        backend.fail_answer = false;
        {
          auto runner = bootstrap::AgentSession::create(options);
          REQUIRE(runner);
          auto completed = co_await (*runner)->run_prompt({.prompt = "Continue"});
          REQUIRE(completed);
        }
        REQUIRE(backend.summaries > 2);
        auto checkpoint = co_await store->load_context(session_id, agent_key);
        REQUIRE(checkpoint);
        REQUIRE(checkpoint->message_count == 182);
        REQUIRE(checkpoint->checkpoint.covered_sequence > 128);
        REQUIRE(checkpoint->checkpoint.summary.contains("Preserve dragonfruit"));
        {
          auto reopened = bootstrap::AgentSession::create(options);
          REQUIRE(reopened);
          auto continued = co_await (*reopened)->run_prompt({.prompt = "What remains?"});
          REQUIRE(continued);
        }
        auto all = co_await store->load(session_id, agent_key);
        REQUIRE(all);
        REQUIRE(all->size() == 184);
        REQUIRE(all->front().blocks == history.front().blocks);
        REQUIRE(std::get<core::TextContent>((*all)[180].blocks.front()).text == "Continue");
      },
      std::chrono::seconds{10});
}

TEST_CASE("Tool iterations compact provisional work and persist it only on success", "[bootstrap][context]") {
  for (const bool cancel : {false, true}) {
    TempDir temp{"oran-tool-context"};
    test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
      class ToolContextProvider final : public provider::System {
      public:
        mutable int calls{};
        mutable int summaries{};
        bool cancel{};
        async::Awaitable<core::Result<provider::Response>>
        send(provider::Request request, provider::ModelTarget, provider::EventSink*) const override {
          if (request.system_prompt.value_or("").starts_with("Produce a bounded session handoff")) {
            ++summaries;
            if (cancel && calls == 2)
              co_return std::unexpected(core::Error::cancelled());
            co_return text_response(
                "Objective:\nFinish task.\nConstraints:\nNo dependencies.\nDecisions:\nUse existing "
                "code.\nCompleted:\nRead tool results.\nPending:\nVerify.\nReferences:\nTool results.\n");
          }
          ++calls;
          if (calls <= 2)
            co_return tool_response("LargeResult", std::to_string(calls), "{}");
          REQUIRE(tool_result_output_in(request, "2").size() == 18000);
          co_return text_response("done");
        }
      } backend;
      backend.cancel = cancel;
      auto cfg = parse_config(
          R"({"runtime":{"prompt":{"active_tools":["LargeResult"]}},"permissions":{"allow":[{"tool_pattern":"LargeResult"}]}})");
      auto assembly = build_assembly(temp.path(), io, false, true, false);
      tool::Registry registry;
      auto added =
          registry.add(core::ToolDef::with_no_input("LargeResult", "Read large context"),
                       [](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
                         co_return tool::Output::text_only(std::string(18000, 'x'));
                       });
      REQUIRE(added);
      agent::ToolScheduler scheduler{io.get_executor(), registry};
      auto id = core::generate_turn_id();
      REQUIRE(id);
      memory::session::SessionId session_id{core::format_turn_id_hex(*id)};
      memory::session::AgentKey agent_key{"coder"};
      std::vector<core::Message> history;
      for (int i = 0; i < 6; ++i) {
        history.push_back(core::Message::user_text(std::string(450, 'u')));
        history.push_back(core::Message::assistant_text(std::string(450, 'a')));
      }
      auto seeded = co_await assembly.session_store()->append_all(session_id, agent_key, history);
      REQUIRE(seeded);
      auto options = base_runner_options(io, assembly, cfg, backend);
      options.session_id = *id;
      options.registry = &registry;
      options.scheduler = &scheduler;
      options.system_preamble = "Agent";
      options.max_tokens = 512;
      options.context = {.max_tokens = 32768, .summary_max_bytes = 512};
      auto runner = bootstrap::AgentSession::create(options);
      REQUIRE(runner);
      auto result = co_await (*runner)->run_prompt({.prompt = "Read twice and finish"});
      auto stored = co_await assembly.session_store()->load_context(session_id, agent_key);
      REQUIRE(stored);
      REQUIRE(backend.summaries >= 2);
      if (cancel) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().kind() == core::ErrorKind::cancelled);
        REQUIRE(stored->message_count == 12);
        REQUIRE(stored->checkpoint.covered_sequence == 0);
      } else {
        REQUIRE(result);
        REQUIRE(stored->message_count == 18);
        REQUIRE(stored->checkpoint.covered_sequence > 12);
        auto messages = co_await assembly.session_store()->load(session_id, agent_key);
        REQUIRE(messages);
        REQUIRE(std::get<core::ToolResultContent>((*messages)[14].blocks.front()).output.size() == 18000);
      }
    });
  }
}

TEST_CASE("Channel adapters drive an AgentSession through the injected turn port", "[bootstrap][channel]") {
  TempDir temp{"oran-channel-session"};
  test::run_async([&temp](asio::io_context& io) -> async::Awaitable<void> {
    auto cfg = config::Config{};
    auto assembly = build_assembly(temp.path(), io, false, false, false);
    provider::FakeProvider fake{{provider::ScriptedTurn{.response = text_response("channel answer"),
                                                        .deltas = {},
                                                        .error = std::nullopt,
                                                        .latency = {}}}};
    auto session = bootstrap::AgentSession::create(base_runner_options(io, assembly, cfg, fake));
    REQUIRE(session);
    hook::Bus bus;
    orangutan::channel::DispatcherOptions options;
    options.hooks = &bus;
    options.mode = permission::Mode::permissive;
    std::string sent;
    auto dispatcher = orangutan::channel::Dispatcher::create(
        orangutan::channel::qq(),
        [&](orangutan::channel::Conversation,
            orangutan::channel::Request request) -> async::Awaitable<core::Result<orangutan::channel::Response>> {
          sent = request.body;
          co_return orangutan::channel::Response{200, R"({"id":"sent"})", {}};
        },
        [&](orangutan::channel::Message message) -> async::Awaitable<core::Result<std::string>> {
          auto result = co_await (*session)->run_prompt({.prompt = message.text});
          if (!result)
            co_return std::unexpected(result.error());
          co_return result->text;
        },
        std::move(options));
    REQUIRE(dispatcher);
    auto incoming = orangutan::channel::qq().decode(
        R"({"op":0,"t":"C2C_MESSAGE_CREATE","d":{"id":"incoming","content":"question","author":{"user_openid":"user"}}})",
        {"account", "bot"});
    REQUIRE(incoming);
    REQUIRE(*incoming);
    auto delivered = co_await (*dispatcher)->handle(**incoming);
    REQUIRE(delivered);
    CHECK(delivered->parts_sent == 1);
    CHECK(sent.contains("channel answer"));
    CHECK(fake.turns_consumed() == 1);
  });
}
