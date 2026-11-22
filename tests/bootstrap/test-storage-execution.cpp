#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/strand.hpp>
#include <catch2/catch_test_macros.hpp>

#include <oran/agent/loop.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config/config.hpp>
#include <oran/provider/fake.hpp>
#include <oran/storage/sqlite.hpp>
#include <oran/tool/registry.hpp>

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace bootstrap = orangutan::bootstrap;
namespace config = orangutan::config;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

class StorageExecution {
public:
  StorageExecution()
      : workspace_{
            std::filesystem::temp_directory_path() /
            ("oran-storage-execution-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directory(workspace_);
    auto built = bootstrap::RuntimeAssembly::build(
        workspace_.string(),
        worker.get_executor(),
        bootstrap::RuntimeAssemblyOptions{.session_memory_enabled = false, .longterm_memory_enabled = false});
    REQUIRE(built.has_value());
    assembly = std::make_unique<bootstrap::RuntimeAssembly>(std::move(*built));
    auto opened = storage::Connection::open(storage::ConnectionOptions{.path = std::string{assembly->audit_path()}});
    REQUIRE(opened.has_value());
    observer = std::move(*opened);
  }

  ~StorageExecution() {
    observer.close();
    assembly.reset();
    std::error_code error;
    std::filesystem::remove_all(workspace_, error);
  }

  template <typename Fn>
  void start(Fn fn) {
    asio::co_spawn(caller,
                   std::move(fn),
                   asio::bind_cancellation_slot(cancellation.slot(), [this](std::exception_ptr ep) {
                     failure = ep;
                     finished = true;
                   }));
  }

  static void poll(asio::io_context& context) {
    context.restart();
    context.poll();
  }

  void finish() {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!finished && std::chrono::steady_clock::now() < deadline) {
      poll(worker);
      poll(coordinator);
    }
    REQUIRE(finished);
    poll(worker);
    poll(coordinator);
    REQUIRE_FALSE(failure);
  }

  std::optional<std::string> cell(std::string_view sql) {
    auto result = observer.query(sql);
    REQUIRE(result.has_value());
    REQUIRE(result->rows.size() == 1);
    REQUIRE(result->rows[0].values.size() == 1);
    return result->rows[0].values[0];
  }

  std::unique_ptr<bootstrap::AgentSession> session(provider::System& provider) {
    auto created = bootstrap::AgentSession::create(bootstrap::AgentSessionOptions{
        .executor = caller,
        .blocking_executor = worker.get_executor(),
        .assembly = assembly.get(),
        .config = &configuration,
        .provider = &provider,
        .route =
            provider::Route{.primary = provider::ModelTarget{.profile = "fake",
                                                             .model = "fake-1",
                                                             .protocol = provider::ProtocolKind::anthropic_messages,
                                                             .thinking_budget = std::nullopt,
                                                             .cache = std::nullopt},
                            .fallbacks = {}},
    });
    REQUIRE(created.has_value());
    return std::move(*created);
  }

  std::filesystem::path workspace_;
  asio::io_context coordinator;
  asio::io_context worker;
  asio::strand<asio::io_context::executor_type> caller{asio::make_strand(coordinator)};
  asio::cancellation_signal cancellation;
  std::unique_ptr<bootstrap::RuntimeAssembly> assembly;
  storage::Connection observer;
  config::Config configuration;
  std::exception_ptr failure;
  bool finished{false};
};

core::ToolDef effect_tool() {
  return core::ToolDef{
      .name = "RecordEffect",
      .description = "Record a controlled effect",
      .input_schema_json = R"({"type":"object","additionalProperties":false})",
      .required_capabilities = {core::Capability::write_file},
  };
}

provider::ScriptedTurn text_turn() {
  return provider::ScriptedTurn{.response =
                                    provider::Response{
                                        .blocks = {core::TextContent{.text = "done"}},
                                        .stop_reason = core::StopReason::end_turn,
                                        .model_used = std::nullopt,
                                    },
                                .deltas = {},
                                .error = std::nullopt};
}

}  // namespace

TEST_CASE("Audit dispatch awaits the worker commit before the effect", "[bootstrap][storage][execution]") {
  StorageExecution runtime;
  tool::Registry registry;
  int effects = 0;
  bool handler_on_caller = false;
  REQUIRE(registry
              .add(effect_tool(),
                   [&](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
                     handler_on_caller = runtime.caller.running_in_this_thread();
                     ++effects;
                     co_return tool::Output::text_only("effect recorded");
                   })
              .has_value());
  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "RecordEffect"});
  auto context = tool::DispatchContext::for_now(runtime.worker.get_executor(),
                                                rules,
                                                runtime.assembly->audit_sink(),
                                                "scope",
                                                "agent",
                                                "owner");
  std::optional<core::Result<tool::Output>> output;
  bool completed_on_caller = false;
  runtime.start([&]() -> async::Awaitable<void> {
    output = co_await registry.dispatch("RecordEffect", "{}", context);
    completed_on_caller = runtime.caller.running_in_this_thread();
  });

  StorageExecution::poll(runtime.coordinator);
  CHECK_FALSE(runtime.finished);
  CHECK(effects == 0);
  CHECK(runtime.cell("SELECT COUNT(*) FROM audit_events") == "0");

  StorageExecution::poll(runtime.worker);
  CHECK(runtime.cell("SELECT MAX(outcome) FROM audit_events") == "allow");
  CHECK(effects == 0);
  CHECK_FALSE(runtime.finished);

  StorageExecution::poll(runtime.coordinator);
  CHECK(effects == 1);
  CHECK(handler_on_caller);

  runtime.finish();
  REQUIRE(output.has_value());
  REQUIRE(output->has_value());
  CHECK((*output)->text == "effect recorded");
  CHECK(completed_on_caller);
  CHECK(runtime.cell("SELECT COUNT(*) FROM audit_events") == "1");
}

TEST_CASE("An audit commit failure prevents an authorized tool effect", "[bootstrap][storage][execution]") {
  StorageExecution runtime;
  auto trigger = runtime.observer.execute(
      "CREATE TRIGGER reject_audit BEFORE INSERT ON audit_events BEGIN SELECT RAISE(ABORT, 'audit unavailable'); END");
  REQUIRE(trigger.has_value());
  tool::Registry registry;
  int effects = 0;
  REQUIRE(registry
              .add(effect_tool(),
                   [&](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
                     ++effects;
                     co_return tool::Output::text_only("effect recorded");
                   })
              .has_value());
  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "RecordEffect"});
  auto context = tool::DispatchContext::for_now(runtime.worker.get_executor(),
                                                rules,
                                                runtime.assembly->audit_sink(),
                                                "scope",
                                                "agent",
                                                "owner");
  std::optional<core::Result<tool::Output>> output;
  runtime.start(
      [&]() -> async::Awaitable<void> { output = co_await registry.dispatch("RecordEffect", "{}", context); });

  runtime.finish();

  REQUIRE(output.has_value());
  REQUIRE_FALSE(output->has_value());
  CHECK(output->error().kind() == core::ErrorKind::storage);
  CHECK(effects == 0);
  CHECK(runtime.cell("SELECT COUNT(*) FROM audit_events") == "0");
}

TEST_CASE("Cancelling a queued audit joins worker completion before dispatch returns",
          "[bootstrap][storage][execution][cancellation]") {
  StorageExecution runtime;
  tool::Registry registry;
  int effects = 0;
  REQUIRE(registry
              .add(effect_tool(),
                   [&](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
                     ++effects;
                     co_return tool::Output::text_only("effect recorded");
                   })
              .has_value());
  permission::RuleSet rules;
  rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "RecordEffect"});
  auto context = tool::DispatchContext::for_now(runtime.worker.get_executor(),
                                                rules,
                                                runtime.assembly->audit_sink(),
                                                "scope",
                                                "agent",
                                                "owner");
  std::optional<core::Result<tool::Output>> output;
  runtime.start(
      [&]() -> async::Awaitable<void> { output = co_await registry.dispatch("RecordEffect", "{}", context); });

  StorageExecution::poll(runtime.coordinator);
  runtime.cancellation.emit(asio::cancellation_type::terminal);
  StorageExecution::poll(runtime.coordinator);
  CHECK_FALSE(runtime.finished);
  CHECK(effects == 0);

  runtime.finish();

  REQUIRE(output.has_value());
  REQUIRE_FALSE(output->has_value());
  CHECK(output->error().kind() == core::ErrorKind::cancelled);
  CHECK(effects == 0);
}

TEST_CASE("A session awaits its terminal trace on the worker executor", "[bootstrap][storage][execution][trace]") {
  StorageExecution runtime;
  provider::FakeProvider provider{{text_turn()}};
  auto session = runtime.session(provider);
  std::optional<core::Result<agent::PromptResult>> result;
  bool resumed_on_caller = false;
  runtime.start([&]() -> async::Awaitable<void> {
    result = co_await session->run_prompt(agent::PromptRequest{.prompt = "hello"});
    resumed_on_caller = runtime.caller.running_in_this_thread();
  });

  StorageExecution::poll(runtime.coordinator);
  CHECK(provider.turns_consumed() == 1);
  CHECK_FALSE(runtime.finished);
  CHECK(runtime.cell("SELECT COUNT(*) FROM trace_turns") == "0");
  StorageExecution::poll(runtime.worker);
  CHECK(runtime.cell("SELECT MAX(stop_reason) FROM trace_turns") == "end_turn");
  CHECK_FALSE(runtime.finished);

  runtime.finish();

  REQUIRE(result.has_value());
  REQUIRE(result->has_value());
  CHECK((*result)->text == "done");
  CHECK(resumed_on_caller);
}

TEST_CASE("A cancelled session retains its services until the terminal trace commits",
          "[bootstrap][storage][execution][trace][cancellation]") {
  StorageExecution runtime;
  auto turn = text_turn();
  turn.latency = 1h;
  provider::FakeProvider provider{{std::move(turn)}};
  auto session = runtime.session(provider);
  std::optional<core::Result<agent::PromptResult>> result;
  runtime.start([&]() -> async::Awaitable<void> {
    result = co_await session->run_prompt(agent::PromptRequest{.prompt = "hello"});
  });

  StorageExecution::poll(runtime.coordinator);
  CHECK(provider.turns_consumed() == 1);
  runtime.cancellation.emit(asio::cancellation_type::terminal);
  StorageExecution::poll(runtime.coordinator);
  CHECK_FALSE(runtime.finished);
  CHECK(runtime.cell("SELECT COUNT(*) FROM trace_turns") == "0");
  runtime.cancellation.emit(asio::cancellation_type::terminal);
  StorageExecution::poll(runtime.coordinator);
  CHECK_FALSE(runtime.finished);
  StorageExecution::poll(runtime.worker);
  CHECK(runtime.cell("SELECT MAX(stop_reason) FROM trace_turns") == "cancelled");
  CHECK_FALSE(runtime.finished);

  runtime.finish();
  session.reset();

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  CHECK(result->error().kind() == core::ErrorKind::cancelled);
  CHECK(runtime.cell("SELECT cancellation_phase FROM trace_turns") == "provider_initial");
  CHECK(runtime.cell("SELECT COUNT(*) FROM trace_turns") == "1");
}

TEST_CASE("Cancellation after trace commit is returned to the loop caller",
          "[bootstrap][storage][execution][trace][cancellation]") {
  StorageExecution runtime;
  provider::FakeProvider provider{{text_turn()}};
  provider::Route route{
      .primary = {.profile = "fake", .model = "fake-1", .thinking_budget = std::nullopt, .cache = std::nullopt},
      .fallbacks = {}};
  agent::Loop loop{provider, std::move(route)};
  auto session_id = core::generate_turn_id();
  REQUIRE(session_id.has_value());
  const std::vector<core::Message> conversation{core::Message::user_text("hello")};
  auto inputs = agent::RunTurnInputs{.conversation_tail = conversation,
                                     .trace = {.repository = runtime.assembly->trace_repository(),
                                               .blocking_executor = runtime.worker.get_executor(),
                                               .session_id = *session_id,
                                               .agent_key = "agent",
                                               .origin = "test"}};
  std::optional<core::Result<agent::RunTurnResult>> result;
  runtime.start([&]() -> async::Awaitable<void> { result = co_await loop.run_turn(inputs); });

  StorageExecution::poll(runtime.coordinator);
  CHECK(provider.turns_consumed() == 1);
  CHECK_FALSE(runtime.finished);
  StorageExecution::poll(runtime.worker);
  CHECK(runtime.cell("SELECT MAX(stop_reason) FROM trace_turns") == "end_turn");
  CHECK_FALSE(runtime.finished);
  runtime.cancellation.emit(asio::cancellation_type::terminal);

  runtime.finish();

  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  CHECK(result->error().kind() == core::ErrorKind::cancelled);
}
