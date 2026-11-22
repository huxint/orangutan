#include <oran/agent.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/async.hpp>
#include <oran/core/content.hpp>
#include <oran/core/error.hpp>
#include <oran/core/message.hpp>
#include <oran/core/role.hpp>
#include <oran/core/stop_reason.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/hook.hpp>
#include <oran/permission.hpp>
#include <oran/prompt.hpp>
#include <oran/provider.hpp>
#include <oran/storage.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace agent = orangutan::agent;
namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace permission = orangutan::permission;
namespace prompt = orangutan::prompt;
namespace provider = orangutan::provider;
namespace storage = orangutan::storage;
namespace test = orangutan::tests;
namespace tool = orangutan::tool;

namespace {

using namespace std::chrono_literals;

struct ProviderHookCapture {
  hook::Event event;
  hook::Payload payload;
};

provider::Route default_route(provider::PromptCacheOptions cache = {}) {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "fake",
              .model = "fake-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = cache,
          },
      .fallbacks = {},
  };
}

provider::Route fallback_route() {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "primary",
              .model = "primary-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
          },
      .fallbacks = {provider::ModelTarget{
          .profile = "fallback",
          .model = "fallback-1",
          .protocol = provider::ProtocolKind::openai_responses,
          .thinking_budget = std::nullopt,
          .cache = std::nullopt,
      }},
  };
}

provider::Route priced_route() {
  return provider::Route{
      .primary =
          provider::ModelTarget{
              .profile = "priced",
              .model = "priced-1",
              .protocol = provider::ProtocolKind::anthropic_messages,
              .thinking_budget = std::nullopt,
              .cache = std::nullopt,
              .pricing =
                  provider::ProviderPricing{
                      .input_per_million_usd = 1.0,
                      .output_per_million_usd = 2.0,
                      .cache_creation_per_million_usd = 3.0,
                      .cache_read_per_million_usd = 0.5,
                  },
          },
      .fallbacks = {},
  };
}

core::ToolDef tool_def(std::string name, std::string description) {
  return core::ToolDef{
      .name = std::move(name),
      .description = std::move(description),
      .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
      .required_capabilities = {},
  };
}

core::ToolDef canned_tool_def(std::string name) {
  auto def = tool_def(std::move(name), "Return a canned result");
  def.input_schema_json = R"({"type":"object","properties":{"value":{"type":"string"}},"additionalProperties":false})";
  return def;
}

void add_canned_tool(tool::Registry& registry,
                     core::ToolDef def,
                     std::string output,
                     bool output_error = false,
                     std::optional<core::ErrorKind> error_kind = std::nullopt) {
  auto handler = [output = std::move(output), output_error, error_kind](
                     std::string_view input,
                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    if (error_kind.has_value()) {
      co_return std::unexpected(core::Error{*error_kind, output});
    }
    const auto text = output + ":" + std::string{input};
    if (output_error) {
      co_return tool::Output::error(text);
    }
    co_return tool::Output::text_only(text);
  };
  REQUIRE(registry.add(std::move(def), std::move(handler)).has_value());
}

void add_structured_tool(tool::Registry& registry, core::ToolDef def, std::string output, std::string data_json) {
  auto handler = [output = std::move(output), data_json = std::move(data_json)](
                     std::string_view input,
                     tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
    co_return tool::Output{
        .text = output + ":" + std::string{input},
        .data_json = data_json,
        .attachments = {},
        .usage = {},
        .is_error = false,
    };
  };
  REQUIRE(registry.add(std::move(def), std::move(handler)).has_value());
}

const core::ToolResultContent& tool_result_at(const core::Message& message, std::size_t index) {
  REQUIRE(index < message.blocks.size());
  const auto* result = std::get_if<core::ToolResultContent>(&message.blocks[index]);
  REQUIRE(result != nullptr);
  return *result;
}

const core::ToolUseContent& tool_use_at(const core::Message& message, std::size_t index) {
  REQUIRE(index < message.blocks.size());
  const auto* result = std::get_if<core::ToolUseContent>(&message.blocks[index]);
  REQUIRE(result != nullptr);
  return *result;
}

bool contains_context(const core::Error& error, std::string_view key, std::string_view value) {
  return std::ranges::any_of(error.context(),
                             [&](const auto& entry) { return entry.first == key && entry.second == value; });
}

std::string tool_result_output_in(const provider::Request& request, std::string_view tool_use_id) {
  for (const auto& message : request.messages) {
    for (const auto& block : message.blocks) {
      if (const auto* result = std::get_if<core::ToolResultContent>(&block);
          result != nullptr && result->tool_use_id == tool_use_id) {
        return result->output;
      }
    }
  }
  return {};
}

permission::RuleSet allow_all_rules() {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = "*",
      .capability = std::nullopt,
  });
  return rules;
}

permission::RuleSet ask_file_read_rules() {
  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::ask,
      .tool_pattern = "FileRead",
      .replay_max = 2,
      .approval_ttl = 30s,
  });
  return rules;
}

permission::ApprovalBroker make_broker() {
  auto broker = permission::ApprovalBroker::with_random_secret();
  REQUIRE(broker.has_value());
  return std::move(*broker);
}

core::TurnId turn_id_with(unsigned char seed) {
  core::TurnId id{};
  for (std::size_t i = 0; i < id.size(); ++i) {
    id[i] = static_cast<std::byte>(seed + i);
  }
  return id;
}

class TempDb {
public:
  explicit TempDb(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".db")) {}

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] std::string string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

storage::Pool open_trace_pool(asio::io_context& io, TempDb& db) {
  auto pool =
      storage::Pool::open(io.get_executor(),
                          storage::PoolOptions{.path = db.string(), .reader_count = 2, .statement_cache_capacity = 8});
  REQUIRE(pool.has_value());
  return std::move(*pool);
}


tool::DispatchContext dispatch_context(asio::io_context& io,
                                       permission::RuleSet& rules,
                                       permission::AuditSink& audit,
                                       std::string identity = "operator-1") {
  return tool::DispatchContext{
      .executor = io.get_executor(),
      .mode = permission::Mode::strict,
      .rules = rules,
      .audit = audit,
      .scope_key = "scope-A",
      .agent_key = "coder",
      .identity = std::move(identity),
  };
}

std::vector<core::ToolDef> loop_catalog() {
  return {
      tool_def("MemoryRecall", "Recall memory"),
      canned_tool_def("FileRead"),
      tool_def("CustomTool", "A registered host extension"),
  };
}

agent::RunTurnInputs base_inputs(const std::vector<core::ToolDef>& catalog, const std::vector<core::Message>& tail) {
  return agent::RunTurnInputs{
      .system_preamble = "system: deterministic test preamble",
      .tool_catalog = catalog,
      .skills_catalog = "skills: none",
      .memory_framing = "memory: none",
      .per_agent_overlay = "overlay: coder",
      .conversation_tail = tail,
      .tool_choice = std::string{"auto"},
      .max_tokens = 512,
      .thinking_budget = std::nullopt,
      .retry = provider::RetryPolicy{},
      .stream = true,
  };
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

/// RecordingProvider is a narrow white-box fixture for Loop's request mapping.
/// FakeProvider proves the scripted provider contract; this fixture proves the
/// loop sends the exact domain request future real adapters will consume.
class RecordingProvider final : public provider::System {
public:
  explicit RecordingProvider(provider::Response response) : response_{std::move(response)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink = nullptr) const override {
    request_ = std::move(request);
    target_ = std::move(target);
    ++calls_;
    if (sink != nullptr) {
      sink->on_done(response_.stop_reason);
    }
    co_return response_;
  }

  [[nodiscard]] const std::optional<provider::Request>& request() const noexcept {
    return request_;
  }

  [[nodiscard]] const std::optional<provider::ModelTarget>& target() const noexcept {
    return target_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  provider::Response response_;
  mutable std::optional<provider::Request> request_;
  mutable std::optional<provider::ModelTarget> target_;
  mutable std::size_t calls_{0};
};

class RecordingSequenceProvider final : public provider::System {
public:
  explicit RecordingSequenceProvider(std::vector<provider::Response> responses) : responses_{std::move(responses)} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink = nullptr) const override {
    requests_.push_back(std::move(request));
    static_cast<void>(target);
    const auto index = cursor_++;
    if (index >= responses_.size()) {
      co_return std::unexpected(core::Error::internal("provider plan exhausted"));
    }
    if (sink != nullptr) {
      sink->on_done(responses_[index].stop_reason);
    }
    co_return responses_[index];
  }

  [[nodiscard]] std::span<const provider::Request> requests() const noexcept {
    return requests_;
  }

private:
  std::vector<provider::Response> responses_;
  mutable std::vector<provider::Request> requests_;
  mutable std::size_t cursor_{0};
};

enum class ProviderCancellationPoint {
  initial,
  stream,
  complete,
};

class ProviderPhaseCancellationProvider final : public provider::System {
public:
  explicit ProviderPhaseCancellationProvider(ProviderCancellationPoint point) : point_{point} {}

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink = nullptr) const override {
    static_cast<void>(request);
    static_cast<void>(target);

    ++calls_;
    switch (point_) {
      case ProviderCancellationPoint::initial:
        break;
      case ProviderCancellationPoint::stream:
        if (sink != nullptr) {
          sink->on_text_delta("partial");
        }
        break;
      case ProviderCancellationPoint::complete:
        if (sink != nullptr) {
          sink->on_done(core::StopReason::end_turn);
        }
        break;
    }
    waiting_ = true;

    auto executor = co_await asio::this_coro::executor;
    auto slept = co_await async::sleep_for(executor, 1s);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }

    co_return provider::Response{
        .blocks = {core::TextContent{.text = "late"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::string{"phase-model"},
    };
  }

  [[nodiscard]] bool waiting() const noexcept {
    return waiting_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

private:
  ProviderCancellationPoint point_;
  mutable bool waiting_{false};
  mutable std::size_t calls_{0};
};

}  // namespace

TEST_CASE("Loop returns text from a single fake-provider end_turn", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "done"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 7,
                                         .output_tokens = 2,
                                         .cache_creation_tokens = 0,
                                         .cache_read_tokens = 0,
                                         .cost_estimate = std::nullopt},
                .model_used = std::string{"fake-1"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");
    REQUIRE(result->stop_reason == core::StopReason::end_turn);
    REQUIRE(result->usage.input_tokens == 7);
    REQUIRE(result->usage.output_tokens == 2);
    REQUIRE(result->model_used == std::string{"fake-1"});
    REQUIRE(result->iterations == 1);
    REQUIRE(result->assistant_blocks.size() == 1);
    REQUIRE(result->rendered_prompt.system_prompt ==
            "system: deterministic test preamble\nskills: none\nmemory: none\noverlay: coder");
    REQUIRE(fake.turns_consumed() == 1);
  });
}

TEST_CASE("Loop maps prompt, messages, active tools, and cache hints into the provider request",
          "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider provider{provider::Response{
        .blocks = {core::TextContent{.text = "mapped"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    agent::Loop loop{provider, default_route(provider::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1})};

    const auto catalog = loop_catalog();
    const std::vector<std::string> selected{"MemoryRecall", "FileRead"};
    const std::vector<core::Message> tail{core::Message::user_text("map this")};
    auto inputs = base_inputs(catalog, tail);
    inputs.active_tools = selected;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.calls() == 1);
    REQUIRE(provider.request().has_value());

    const auto& request = *provider.request();
    REQUIRE(request.messages == tail);
    REQUIRE(request.stream);
    REQUIRE(request.tool_choice == std::string{"auto"});
    REQUIRE(request.max_tokens == 512);
    REQUIRE(request.system_prompt.has_value());
    REQUIRE(*request.system_prompt == result->rendered_prompt.system_prompt);
    REQUIRE(request.system_prompt->contains("system: deterministic test preamble"));
    REQUIRE_FALSE(request.system_prompt->contains("Tool: FileRead"));
    REQUIRE_FALSE(request.system_prompt->contains("Tool: MemoryRecall"));
    REQUIRE_FALSE(request.system_prompt->contains("Tool: CustomTool"));

    REQUIRE(request.tools.size() == 2);
    REQUIRE(request.tools[0].name == "FileRead");
    REQUIRE(request.tools[1].name == "MemoryRecall");
    REQUIRE(request.cache.has_value());
    REQUIRE(request.cache->prefix_bytes == result->rendered_prompt.prefix_bytes);
    REQUIRE(provider.target().has_value());
    REQUIRE(provider.target()->model == "fake-1");
  });
}

TEST_CASE("Loop sends each native schema once in both supported protocols", "[unit][agent][loop][protocol]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider recording{provider::Response{
        .blocks = {core::TextContent{.text = "done"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    agent::Loop loop{recording, default_route()};
    const std::vector<core::ToolDef> catalog{core::ToolDef{
        .name = "CustomLookup",
        .description = "native_description_probe",
        .input_schema_json = R"({"type":"object","properties":{"native_schema_probe":{"type":"string"}}})",
        .required_capabilities = {},
    }};
    const std::vector<core::Message> tail{core::Message::user_text("Inspect the available context.")};
    const auto result = co_await loop.run_turn(base_inputs(catalog, tail));
    REQUIRE(result.has_value());
    REQUIRE(recording.request().has_value());
    const auto& request = *recording.request();
    REQUIRE(request.tools == catalog);
    REQUIRE(request.system_prompt.has_value());
    REQUIRE_FALSE(request.system_prompt->contains("native_schema_probe"));
    REQUIRE_FALSE(request.system_prompt->contains("native_description_probe"));

    for (const auto protocol : {provider::ProtocolKind::anthropic_messages, provider::ProtocolKind::openai_responses}) {
      auto target = default_route().primary;
      target.protocol = protocol;
      const auto encoded = provider::make_protocol_request(request, target);
      REQUIRE(encoded.has_value());
      for (const std::string_view marker : {"native_schema_probe", "native_description_probe"}) {
        auto parts = std::string_view{encoded->body_json} | std::views::split(marker);
        REQUIRE(std::ranges::distance(parts) == 2);
      }
    }
  });
}

TEST_CASE("Loop validates explicit tool selection before provider execution", "[unit][agent][loop][catalog]") {
  bool unknown = false;
  SECTION("explicit empty selection") {}
  SECTION("unknown tool name") {
    unknown = true;
  }
  test::run_async([unknown](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider recording{provider::Response{
        .blocks = {core::TextContent{.text = "done"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    agent::Loop loop{recording, default_route()};
    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("Hello")};
    const std::vector<std::string> names =
        unknown ? std::vector<std::string>{"MissingTool"} : std::vector<std::string>{};
    auto inputs = base_inputs(catalog, tail);
    inputs.active_tools = names;
    const auto result = co_await loop.run_turn(inputs);
    if (unknown) {
      REQUIRE_FALSE(result.has_value());
      REQUIRE(result.error().kind() == core::ErrorKind::not_found);
      REQUIRE(recording.calls() == 0);
    } else {
      REQUIRE(result.has_value());
      REQUIRE(recording.request().has_value());
      REQUIRE(recording.request()->tools.empty());
    }
  });
}

TEST_CASE("Loop uses the stable default system preamble when no override is supplied", "[unit][agent][loop][prompt]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{{
        provider::Response{
            .blocks =
                {core::ToolUseContent{.id = "read-1", .name = "FileRead", .input_json = R"({"path":"note.txt"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "defaulted"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    auto loop =
        agent::Loop{provider, default_route(provider::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1})};
    auto registry = tool::Registry{};
    add_canned_tool(registry, canned_tool_def("FileRead"), "read ok");
    permission::NullAuditSink audit;
    permission::RuleSet rules;
    rules.push_back(permission::Rule{.verdict = permission::Verdict::allow, .tool_pattern = "File*"});
    auto context = tool::DispatchContext{
        .executor = co_await asio::this_coro::executor,
        .mode = permission::Mode::strict,
        .rules = rules,
        .audit = audit,
        .scope_key = "scope-A",
        .agent_key = "coder",
        .identity = "operator-1",
    };

    const auto catalog = loop_catalog();
    const std::vector<core::Message> first_tail{core::Message::user_text("first")};
    auto first_inputs = base_inputs(catalog, first_tail);
    first_inputs.system_preamble = "";
    first_inputs.tools = &registry;
    first_inputs.dispatch_context = &context;

    auto first = co_await loop.run_turn(first_inputs);

    REQUIRE(first.has_value());
    REQUIRE(first->text == "defaulted");
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests()[0].system_prompt.has_value());
    REQUIRE(provider.requests()[1].system_prompt.has_value());
    REQUIRE(provider.requests()[0].system_prompt->contains("You are Orangutan"));
    REQUIRE(provider.requests()[0].system_prompt->contains("Operating principles:"));
    REQUIRE_FALSE(provider.requests()[0].system_prompt->contains("Tool: FileRead"));
    REQUIRE(provider.requests()[0].system_prompt->contains("memory: none"));
    REQUIRE(*provider.requests()[0].system_prompt == *provider.requests()[1].system_prompt);
    REQUIRE(first->rendered_prompt.system_prompt == *provider.requests()[0].system_prompt);

    RecordingProvider second_provider{provider::Response{
        .blocks = {core::TextContent{.text = "second"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    }};
    auto second_loop = agent::Loop{second_provider,
                                   default_route(provider::PromptCacheOptions{.enabled = true, .min_prefix_bytes = 1})};
    const std::vector<core::Message> second_tail{core::Message::user_text("second")};
    auto second_inputs = base_inputs(catalog, second_tail);
    second_inputs.system_preamble = "";

    auto second = co_await second_loop.run_turn(second_inputs);

    REQUIRE(second.has_value());
    REQUIRE(second->rendered_prompt == first->rendered_prompt);
  });
}

TEST_CASE("Loop snapshots stable text for a turn and observes host edits on the next turn",
          "[unit][agent][loop][prompt]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    const auto done = provider::Response{
        .blocks = {core::TextContent{.text = "done"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = {},
        .model_used = std::nullopt,
    };
    RecordingSequenceProvider provider{{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "edit", .name = "UpdateContext", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        done,
        done,
    }};
    agent::Loop loop{provider, default_route()};
    std::array<std::string, 4> text{"system-before", "skills-before", "memory-before", "overlay-before"};
    tool::Registry registry;
    REQUIRE(registry.add(core::ToolDef::with_no_input("UpdateContext", "Update host context"),
                         [&text](std::string_view, tool::DispatchContext&)
                             -> async::Awaitable<core::Result<tool::Output>> {
                           // Keep borrowed views valid while changing their source bytes.
                           for (auto& section : text) {
                             section.front() = '#';
                           }
                           co_return tool::Output::text_only("dynamic tool result");
                         })
                .has_value());
    auto rules = allow_all_rules();
    permission::NullAuditSink audit;
    auto context = dispatch_context(io, rules, audit);
    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("dynamic user message")};
    auto inputs = base_inputs(catalog, tail);
    inputs.system_preamble = text[0];
    inputs.skills_catalog = text[1];
    inputs.memory_framing = text[2];
    inputs.per_agent_overlay = text[3];
    inputs.tools = &registry;
    inputs.dispatch_context = &context;
    const auto first = co_await loop.run_turn(inputs);

    REQUIRE(first.has_value());
    REQUIRE(provider.requests().size() == 2);
    for (const auto& request : provider.requests()) {
      REQUIRE(request.system_prompt == "system-before\nskills-before\nmemory-before\noverlay-before");
      REQUIRE(request.system_prompt == first->rendered_prompt.system_prompt);
      REQUIRE(request.cache.has_value());
      CHECK(request.cache->prefix_hash == first->rendered_prompt.prefix_hash);
      CHECK(request.cache->prefix_bytes == first->rendered_prompt.prefix_bytes);
    }
    REQUIRE(provider.requests()[0].messages == tail);
    REQUIRE(provider.requests()[1].messages.size() == 3);
    REQUIRE(tool_result_output_in(provider.requests()[1], "edit") == "dynamic tool result");
    REQUIRE(first->transcript.size() == 4);

    const std::vector<core::Message> next_tail{core::Message::user_text("next prompt")};
    inputs.conversation_tail = next_tail;
    const auto second = co_await loop.run_turn(inputs);
    REQUIRE(second.has_value());
    REQUIRE(provider.requests().size() == 3);
    REQUIRE(provider.requests()[2].messages == next_tail);
    REQUIRE(provider.requests()[2].system_prompt == "#ystem-before\n#kills-before\n#emory-before\n#verlay-before");
    REQUIRE(second->rendered_prompt.system_prompt == provider.requests()[2].system_prompt);
    REQUIRE(second->rendered_prompt.prefix_hash != first->rendered_prompt.prefix_hash);
    REQUIRE(second->rendered_prompt.prefix_bytes == first->rendered_prompt.prefix_bytes);
  });
}

TEST_CASE("Loop publishes provider request and response hooks", "[unit][agent][loop][hooks]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    RecordingProvider provider{provider::Response{
        .blocks = {core::TextContent{.text = "hooked"}},
        .stop_reason = core::StopReason::end_turn,
        .usage = provider::Usage{.input_tokens = 11,
                                 .output_tokens = 5,
                                 .cache_creation_tokens = 3,
                                 .cache_read_tokens = 2,
                                 .cost_estimate = 0.004},
        .model_used = std::string{"served-model"},
    }};
    agent::Loop loop{provider, default_route()};
    hook::Bus bus;
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    bus.subscribe(sink, {hook::Event::provider_request, hook::Event::provider_response});

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hook this")};
    auto inputs = base_inputs(catalog, tail);
    inputs.bus = &bus;
    inputs.scope_key = "scope-A";
    inputs.agent_key = "coder";
    inputs.identity = "operator-1";
    inputs.origin = "cli";
    inputs.turn_id = turn_id_with(0x12);
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(captures.size() == 2);
    REQUIRE(captures[0].event == hook::Event::provider_request);
    const auto* request = std::get_if<hook::ProviderRequestPayload>(&captures[0].payload);
    REQUIRE(request != nullptr);
    REQUIRE(request->who.scope_key == "scope-A");
    REQUIRE(request->who.agent_key == "coder");
    REQUIRE(request->who.identity == "operator-1");
    REQUIRE(request->origin == "cli");
    REQUIRE(request->turn_id == turn_id_with(0x12));
    REQUIRE(request->iteration == 1);
    REQUIRE(request->route_profile == "fake");
    REQUIRE(request->route_model == "fake-1");
    REQUIRE(request->route_protocol == "anthropic_messages");
    REQUIRE(request->fallback_count == 0);
    REQUIRE(request->message_count == 1);
    REQUIRE(request->tool_count == 3);
    REQUIRE(request->stream);
    REQUIRE(request->max_tokens == std::optional<std::uint32_t>{512});
    REQUIRE(request->thinking_budget == std::nullopt);
    REQUIRE(request->retry_max_attempts == 1);
    REQUIRE(request->retry_initial_backoff == 0ms);

    REQUIRE(captures[1].event == hook::Event::provider_response);
    const auto* response = std::get_if<hook::ProviderResponsePayload>(&captures[1].payload);
    REQUIRE(response != nullptr);
    REQUIRE(response->who.agent_key == "coder");
    REQUIRE(response->turn_id == turn_id_with(0x12));
    REQUIRE(response->iteration == 1);
    REQUIRE(response->route_profile == "fake");
    REQUIRE(response->route_model == "fake-1");
    REQUIRE(response->served_profile == "fake");
    REQUIRE(response->served_model == "served-model");
    REQUIRE(response->served_protocol == "anthropic_messages");
    REQUIRE(response->stop_reason == "end_turn");
    REQUIRE(response->usage.input_tokens == 11);
    REQUIRE(response->usage.output_tokens == 5);
    REQUIRE(response->usage.cache_creation_tokens == 3);
    REQUIRE(response->usage.cache_read_tokens == 2);
    REQUIRE(response->usage.cost_estimate == std::optional<double>{0.004});
    REQUIRE(response->finished_at >= response->started_at);
    REQUIRE(response->duration.count() >= 0);
  });
}

TEST_CASE("Loop publishes provider error hooks", "[unit][agent][loop][hooks]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};
    hook::Bus bus;
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    bus.subscribe(sink, {hook::Event::provider_request, hook::Event::provider_error});

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("fail")};
    auto inputs = base_inputs(catalog, tail);
    inputs.bus = &bus;
    inputs.scope_key = "scope-A";
    inputs.agent_key = "coder";
    inputs.identity = "operator-1";
    inputs.origin = "cli";
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(captures.size() == 2);
    REQUIRE(captures[0].event == hook::Event::provider_request);
    REQUIRE(captures[1].event == hook::Event::provider_error);
    const auto* error = std::get_if<hook::ProviderErrorPayload>(&captures[1].payload);
    REQUIRE(error != nullptr);
    REQUIRE(error->who.agent_key == "coder");
    REQUIRE(error->route_profile == "fake");
    REQUIRE(error->route_model == "fake-1");
    REQUIRE(error->route_protocol == "anthropic_messages");
    REQUIRE(error->error_kind == "network");
    REQUIRE(error->error_message == "upstream timeout");
    REQUIRE(error->retryable);
    REQUIRE(error->finished_at >= error->started_at);
    REQUIRE(error->duration.count() >= 0);
  });
}

TEST_CASE("Loop attributes terminal fallback errors to the failing fallback target", "[unit][agent][loop][hooks]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    class ScriptedBackend final : public provider::System {
    public:
      [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
      send(provider::Request request,
           provider::ModelTarget target,
           provider::EventSink* sink = nullptr) const override {
        static_cast<void>(request);
        static_cast<void>(sink);
        static_cast<void>(target);
        if (cursor_++ == 0) {
          co_return std::unexpected(core::Error::network("primary transient failure"));
        }
        co_return std::unexpected(core::Error{core::ErrorKind::auth, "fallback key rejected"}
                                      .with("provider_profile", "primary")
                                      .with("provider_model", "spoofed-model"));
      }

    private:
      mutable std::size_t cursor_{0};
    };
    ScriptedBackend backend;
    agent::Loop loop{backend, fallback_route()};
    hook::Bus bus;
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    bus.subscribe(sink, {hook::Event::provider_request, hook::Event::provider_error});

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("fail")};
    auto inputs = base_inputs(catalog, tail);
    inputs.bus = &bus;
    inputs.scope_key = "scope-A";
    inputs.agent_key = "coder";
    inputs.identity = "operator-1";
    inputs.origin = "cli";
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    const auto* error = std::get_if<hook::ProviderErrorPayload>(&captures.back().payload);
    REQUIRE(error != nullptr);
    REQUIRE(error->route_profile == "fallback");
    REQUIRE(error->route_model == "fallback-1");
    REQUIRE(error->route_protocol == "openai_responses");
    REQUIRE(error->error_kind == "auth");
  });
}

TEST_CASE("Loop attributes terminal fallback errors to the fallback trace row", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-fallback-error-trace"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    class ScriptedBackend final : public provider::System {
    public:
      [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
      send(provider::Request request,
           provider::ModelTarget target,
           provider::EventSink* sink = nullptr) const override {
        static_cast<void>(request);
        static_cast<void>(sink);
        static_cast<void>(target);
        if (cursor_++ == 0) {
          co_return std::unexpected(core::Error::network("primary transient failure"));
        }
        co_return std::unexpected(core::Error{core::ErrorKind::auth, "fallback key rejected"}
                                      .with("provider_profile", "primary")
                                      .with("provider_model", "spoofed-model"));
      }

    private:
      mutable std::size_t cursor_{0};
    };
    ScriptedBackend backend;
    agent::Loop loop{backend, fallback_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("fail")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x43);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    auto row = co_await trace.get_turn(turn_id_with(0x43));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->route_profile == "fallback");
    REQUIRE((*row)->route_model == "fallback-1");
    REQUIRE((*row)->stop_reason == "error");
    REQUIRE((*row)->iteration_count == 1);
  });
}

TEST_CASE("Loop persists execution cost through response hooks and trace", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-priced-trace"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "priced"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 1'000'000,
                                         .output_tokens = 500'000,
                                         .cache_creation_tokens = 250'000,
                                         .cache_read_tokens = 100'000,
                                         .cost_estimate = std::nullopt},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, priced_route()};
    hook::Bus bus;
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    bus.subscribe(sink, {hook::Event::provider_response});

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("price this")};
    auto inputs = base_inputs(catalog, tail);
    inputs.bus = &bus;
    inputs.turn_id = turn_id_with(0x51);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->usage.cost_estimate == std::optional<double>{2.8});
    REQUIRE(captures.size() == 1);
    const auto* response = std::get_if<hook::ProviderResponsePayload>(&captures[0].payload);
    REQUIRE(response != nullptr);
    REQUIRE(response->usage.cost_estimate == std::optional<double>{2.8});

    auto row = co_await trace.get_turn(turn_id_with(0x51));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->route_profile == "priced");
    REQUIRE((*row)->route_model == "priced-1");
    REQUIRE((*row)->cost_estimate_usd == 2.8);
  });
}

TEST_CASE("Loop preserves provider-supplied usage cost over route pricing", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "provider priced"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 1'000'000,
                                         .output_tokens = 1'000'000,
                                         .cache_creation_tokens = 1'000'000,
                                         .cache_read_tokens = 1'000'000,
                                         .cost_estimate = 0.25},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, priced_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("price this")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE(result.has_value());
    REQUIRE(result->usage.cost_estimate == std::optional<double>{0.25});
  });
}

TEST_CASE("Loop reports the served fallback target on provider_response", "[unit][agent][loop][hooks]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    provider::FakeProvider provider{{
        {.response = std::nullopt, .deltas = {}, .error = core::Error::network("primary unavailable"), .latency = {}},
        {.response =
             provider::Response{
                 .blocks = {core::TextContent{.text = "fallback"}},
                 .stop_reason = core::StopReason::end_turn,
                 .usage = {},
                 .model_used = std::string{"fallback-1"},
             },
         .deltas = {},
         .error = std::nullopt,
         .latency = {}},
    }};
    agent::Loop loop{provider, fallback_route()};
    hook::Bus bus;
    std::vector<ProviderHookCapture> captures;
    auto sink = provider_capture_sink(captures);
    bus.subscribe(sink, {hook::Event::provider_request, hook::Event::provider_response});

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("fallback")};
    auto inputs = base_inputs(catalog, tail);
    inputs.bus = &bus;
    inputs.scope_key = "scope-A";
    inputs.agent_key = "coder";
    inputs.identity = "operator-1";
    inputs.origin = "cli";
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(captures.size() == 2);
    REQUIRE(captures[0].event == hook::Event::provider_request);
    REQUIRE(captures[1].event == hook::Event::provider_response);
    const auto* response = std::get_if<hook::ProviderResponsePayload>(&captures[1].payload);
    REQUIRE(response != nullptr);
    REQUIRE(response->who.agent_key == "coder");
    REQUIRE(response->route_profile == "primary");
    REQUIRE(response->route_model == "primary-1");
    REQUIRE(response->route_protocol == "anthropic_messages");
    REQUIRE(response->served_profile == "fallback");
    REQUIRE(response->served_model == "fallback-1");
    REQUIRE(response->served_protocol == "openai_responses");
  });
}

TEST_CASE("Loop forwards provider errors unchanged", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(result.error().retryable());
  });
}

TEST_CASE("Loop persists provider error trace rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-provider-error"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace provider error")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x43);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(result.error().retryable());

    auto row = co_await trace.get_turn(turn_id_with(0x43));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x43));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_profile == "fake");
    REQUIRE((*row)->route_model == "fake-1");
    REQUIRE((*row)->stop_reason == "error");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 0);
    REQUIRE((*row)->output_tokens == 0);
    REQUIRE((*row)->cache_creation_tokens == 0);
    REQUIRE((*row)->cache_read_tokens == 0);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
  });
}

TEST_CASE("Loop preserves the original error when trace-row write fails", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-write-fail"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response = std::nullopt,
        .deltas = {},
        .error = core::Error::network("upstream timeout"),
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace write fail")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x44);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = core::TurnId{},
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::network);
    REQUIRE(contains_context(result.error(), "trace_write_failed", "trace repository field is invalid"));
  });
}

TEST_CASE("Loop annotates cancellation during provider await", "[unit][agent][loop][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  std::vector<provider::ScriptedTurn> plan;
  plan.push_back(provider::ScriptedTurn{
      .response =
          provider::Response{
              .blocks = {core::TextContent{.text = "late"}},
              .stop_reason = core::StopReason::end_turn,
              .usage = {},
              .model_used = std::nullopt,
          },
      .deltas = {},
      .error = std::nullopt,
      .latency = 1s,
  });
  provider::FakeProvider fake{std::move(plan)};
  agent::Loop loop{fake, default_route()};

  const auto catalog = loop_catalog();
  const std::vector<core::Message> tail{core::Message::user_text("cancel provider")};
  std::optional<core::Result<agent::RunTurnResult>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> {
        co_return co_await loop.run_turn(base_inputs(catalog, tail));
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(contains_context(result->error(), "reason", "parent_cancelled"));
  REQUIRE(contains_context(result->error(), "cancellation_phase", "provider_initial"));
  REQUIRE(fake.turns_consumed() == 1);
}

TEST_CASE("Loop classifies provider streaming cancellation phases", "[unit][agent][loop][trace][cancellation]") {
  TempDb db{"oran-agent-loop-trace-provider-phase-cancel"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    const auto catalog = loop_catalog();
    provider::EventSink sink;

    auto run_phase = [&](ProviderCancellationPoint point,
                         unsigned char turn_seed,
                         std::string_view expected_phase) -> async::Awaitable<void> {
      ProviderPhaseCancellationProvider provider{point};
      agent::Loop loop{provider, default_route()};
      const std::vector<core::Message> tail{core::Message::user_text("cancel provider phase")};
      auto inputs = base_inputs(catalog, tail);
      const auto turn_id = turn_id_with(turn_seed);
      inputs.turn_id = turn_id;
      inputs.trace = agent::TraceContext{
          .repository = &trace,
          .blocking_executor = io.get_executor(),
          .session_id = turn_id_with(0x80),
          .agent_key = "coder",
          .origin = "cli",
      };

      asio::cancellation_signal signal;
      std::optional<core::Result<agent::RunTurnResult>> result;
      std::exception_ptr failure;
      asio::co_spawn(
          io,
          [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> {
            co_return co_await loop.run_turn(inputs, &sink);
          },
          asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
            failure = ep;
            result = std::move(r);
          }));

      while (!provider.waiting() && !result.has_value() && !failure) {
        auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
        REQUIRE(tick.has_value());
      }
      REQUIRE(provider.waiting());
      signal.emit(asio::cancellation_type::terminal);
      while (!result.has_value() && !failure) {
        auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
        REQUIRE(tick.has_value());
      }
      if (failure) {
        std::rethrow_exception(failure);
      }

      REQUIRE(result.has_value());
      REQUIRE_FALSE(result->has_value());
      REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
      REQUIRE(contains_context(result->error(), "cancellation_phase", expected_phase));
      REQUIRE(provider.calls() == 1);

      auto row = co_await trace.get_turn(turn_id);
      REQUIRE(row.has_value());
      REQUIRE(row->has_value());
      REQUIRE((*row)->turn_id == turn_id);
      REQUIRE((*row)->stop_reason == "cancelled");
      REQUIRE((*row)->cancellation_phase.has_value());
      REQUIRE(*(*row)->cancellation_phase == expected_phase);
    };

    co_await run_phase(ProviderCancellationPoint::initial, 0x61, "provider_initial");
    co_await run_phase(ProviderCancellationPoint::stream, 0x62, "provider_stream");
    co_await run_phase(ProviderCancellationPoint::complete, 0x63, "provider_complete");
  });
}

TEST_CASE("Loop rejects tool-use responses without dispatch services", "[unit][agent][loop]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = R"({"path":"x"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = {},
                .model_used = std::nullopt,
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read x")};
    auto result = co_await loop.run_turn(base_inputs(catalog, tail));

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "agent loop: response requires a later loop slice");
  });
}

TEST_CASE("Loop persists loop-boundary error trace rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-boundary-error"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = R"({"path":"x"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = provider::Usage{.input_tokens = 13,
                                         .output_tokens = 2,
                                         .cache_creation_tokens = 1,
                                         .cache_read_tokens = 4,
                                         .cost_estimate = 0.003},
                .model_used = std::string{"fake-boundary-model"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace missing dispatch context")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x44);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "agent loop: response requires a later loop slice");

    auto row = co_await trace.get_turn(turn_id_with(0x44));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x44));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_model == "fake-boundary-model");
    REQUIRE((*row)->stop_reason == "error");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 13);
    REQUIRE((*row)->output_tokens == 2);
    REQUIRE((*row)->cache_creation_tokens == 1);
    REQUIRE((*row)->cache_read_tokens == 4);
    REQUIRE((*row)->cost_estimate_usd == 0.003);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
  });
}

TEST_CASE("Loop rejects incomplete responses before tools or success", "[unit][agent][loop][trace]") {
  const auto reason = GENERATE(core::StopReason::max_tokens, core::StopReason::cancelled, core::StopReason::error);
  const bool with_tool = GENERATE(false, true);
  TempDb db{"oran-incomplete-response"};
  test::run_async([&](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());
    provider::Response response{
        .blocks = {core::TextContent{.text = "partial answer"}},
        .stop_reason = reason,
        .usage = {.input_tokens = 11, .output_tokens = 7, .cost_estimate = std::nullopt},
    };
    if (with_tool) {
      response.blocks.push_back(core::ToolUseContent{.id = "write-1", .name = "Write", .input_json = "{}"});
    }
    RecordingProvider provider{std::move(response)};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    int effects = 0;
    REQUIRE(registry.add(tool_def("Write", "Perform an effect"),
        [&](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
          ++effects;
          co_return tool::Output::text_only("changed");
        }).has_value());
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("perform work")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(effects == 0);
    REQUIRE(audit.events().empty());
    REQUIRE(provider.calls() == 1);
    const bool cancelled = reason == core::StopReason::cancelled;
    REQUIRE(result.error().kind() == (cancelled ? core::ErrorKind::cancelled : core::ErrorKind::upstream));
    if (cancelled) {
      REQUIRE(contains_context(result.error(), "cancellation_phase", "provider_complete"));
    } else {
      REQUIRE(contains_context(result.error(), "stop_reason", core::enum_name(reason)));
    }
    auto row = co_await trace.get_turn(*inputs.turn_id);
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->stop_reason == (cancelled ? "cancelled" : "error"));
    REQUIRE((*row)->input_tokens == 11);
    REQUIRE((*row)->output_tokens == 7);
    REQUIRE((*row)->iteration_count == 1);
  });
}

TEST_CASE("Loop dispatches one tool_use and re-enters the provider with a tool result", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = R"({"value":"a"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 5,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = provider::Usage{.input_tokens = 3,
                                     .output_tokens = 2,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"fake-1"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read-ok");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x20);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");
    REQUIRE(result->iterations == 2);
    REQUIRE(result->usage.input_tokens == 8);
    REQUIRE(result->usage.output_tokens == 3);
    REQUIRE(result->model_used == std::string{"fake-1"});
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests()[0].messages == tail);
    REQUIRE(provider.requests()[1].messages.size() == 3);
    REQUIRE(provider.requests()[1].messages[0] == tail[0]);
    REQUIRE(provider.requests()[1].messages[1].role == core::Role::assistant);
    REQUIRE(tool_use_at(provider.requests()[1].messages[1], 0).id == "t1");
    REQUIRE(provider.requests()[1].messages[2].role == core::Role::tool);
    const auto& tool_result = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(tool_result.tool_use_id == "t1");
    REQUIRE(tool_result.output == R"(read-ok:{"value":"a"})");
    REQUIRE_FALSE(tool_result.is_error);
    REQUIRE(tool_result_output_in(provider.requests()[1], "t1") == R"(read-ok:{"value":"a"})");
    REQUIRE(result->transcript.size() == 4);
    REQUIRE(result->transcript[0] == tail[0]);
    REQUIRE(result->transcript[1] == provider.requests()[1].messages[1]);
    REQUIRE(result->transcript[2] == provider.requests()[1].messages[2]);
    REQUIRE(result->transcript[3].role == core::Role::assistant);
    REQUIRE(std::get<core::TextContent>(result->transcript[3].blocks[0]).text == "final");
    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].tool_name == "FileRead");
    REQUIRE(audit.events()[0].parent_turn_id.has_value());
    REQUIRE(*audit.events()[0].parent_turn_id == turn_id_with(0x20));
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop preserves structured tool output for provider protocol mapping", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "structured-1",
                                            .name = "FileRead",
                                            .input_json = R"({"value":"structured"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_structured_tool(registry,
                        canned_tool_def("FileRead"),
                        "read-ok",
                        R"({"kind":"file_read","path":"README.md","text":"body"})");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    const auto& tool_result = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(tool_result.tool_use_id == "structured-1");
    REQUIRE(tool_result.output == R"(read-ok:{"value":"structured"})");
    REQUIRE(tool_result.data_json ==
            std::optional<std::string>{R"({"kind":"file_read","path":"README.md","text":"body"})"});
    REQUIRE_FALSE(tool_result.is_error);
  });
}

TEST_CASE("Loop exposes incomplete tool results to the provider and transcript", "[unit][agent][loop]") {
  const int cap_mode = GENERATE(0, 1, 2);
  const bool source_truncated = cap_mode == 0;
  const bool output_error = GENERATE(false, true);
  test::run_async([=](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "partial-1", .name = "Read", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "done"}},
            .stop_reason = core::StopReason::end_turn,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    REQUIRE(registry.add(tool_def("Read", "Read bounded text"),
        [=](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
          co_return tool::Output{
              .text = "abcdefgh",
              .data_json = R"({"value":"full"})",
              .usage = {.truncated = source_truncated},
              .is_error = output_error,
          };
        }).has_value());
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    // Exercise both source-declared truncation and dispatch-enforced caps.
    ctx.output_caps = source_truncated
        ? tool::OutputCapOptions{.max_text_bytes = 0, .max_data_bytes = 0}
        : tool::OutputCapOptions{.max_text_bytes = cap_mode == 1 ? 1U : 0U, .max_data_bytes = 1};
    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);
    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    const auto& visible = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(visible.tool_use_id == "partial-1");
    REQUIRE(visible.is_error == output_error);
    REQUIRE(visible.output.starts_with("[Tool output truncated.") == (cap_mode != 2));
    REQUIRE(visible.output.contains("do not assume the displayed result is complete") == (cap_mode != 2));
    REQUIRE(visible.output.ends_with(cap_mode == 1 ? "a" : "abcdefgh"));
    REQUIRE(visible.output.contains("Structured tool data omitted") == !source_truncated);
    REQUIRE(visible.data_json.has_value() == source_truncated);
    const auto& persisted = tool_result_at(result->transcript[2], 0);
    REQUIRE(persisted.output == visible.output);
    REQUIRE(persisted.data_json == visible.data_json);
    REQUIRE(persisted.is_error == visible.is_error);
  });
}

TEST_CASE("Loop refreshes dispatch time for blocking permission approvals", "[unit][agent][loop][approval]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    constexpr std::string_view input = R"({"value":"approve"})";
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "approval-1", .name = "FileRead", .input_json = std::string{input}}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "approved final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::string{"fake-1"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "approved-read");
    auto rules = ask_file_read_rules();
    permission::RecordingAuditSink audit;
    auto broker = make_broker();
    permission::ApprovalToken issued_token{};

    hook::Bus bus;
    std::vector<hook::PermissionAskRenderedPayload> prompts;
    hook::Sink prompt{
        .id = "agent-approval",
        .decide = [&prompts](hook::Event event,
                             hook::PayloadPtr payload) -> async::Awaitable<core::Result<hook::HookDecision>> {
          REQUIRE(event == hook::Event::permission_ask_rendered);
          const auto* ask = std::get_if<hook::PermissionAskRenderedPayload>(payload.get());
          REQUIRE(ask != nullptr);
          prompts.push_back(*ask);

          hook::HookDecision decision{};
          decision.reason = "operator_approved:operator-1";
          co_return decision;
        },
    };
    bus.subscribe(prompt, {hook::Event::permission_ask_rendered});

    auto ctx = dispatch_context(io, rules, audit);
    ctx.approval_broker = &broker;
    ctx.approval_token_output = &issued_token;
    ctx.bus = &bus;
    ctx.now = core::Time::epoch();

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read with approval")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "approved final");
    REQUIRE(result->iterations == 2);
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(tool_result_output_in(provider.requests()[1], "approval-1") == R"(approved-read:{"value":"approve"})");

    REQUIRE(prompts.size() == 1);
    const auto& ask = prompts[0];
    REQUIRE(ask.tool_name == "FileRead");
    REQUIRE(ask.input_json == input);
    REQUIRE(ask.who.scope_key == "scope-A");
    REQUIRE(ask.who.agent_key == "coder");
    REQUIRE(ask.who.identity == "operator-1");
    REQUIRE(ask.decision_reason == "rule #0 (ask: FileRead)");
    REQUIRE(ask.replay_max == 2);
    REQUIRE(ask.approval_ttl == 30s);
    REQUIRE(ask.requested_at > core::Time::epoch());
    REQUIRE(ctx.now == core::Time::epoch());

    REQUIRE(audit.events().size() == 1);
    REQUIRE(audit.events()[0].verdict == permission::Verdict::ask);
    REQUIRE(audit.events()[0].outcome == permission::AuditOutcome::approved);
    REQUIRE(audit.events()[0].metadata_json.contains("permission_ask_decisions"));
    REQUIRE(audit.events()[0].metadata_json.contains("agent-approval"));
    REQUIRE(audit.events()[0].metadata_json.contains("operator_approved:operator-1"));

    auto replay = broker.check(issued_token, "FileRead", input, "operator-1", ask.requested_at);
    REQUIRE(replay.has_value());
  });
}

TEST_CASE("Loop routes tool dispatch through a caller-supplied scheduler", "[unit][agent][loop][scheduler]") {
  test::run_async(
      [](asio::io_context& io) -> async::Awaitable<void> {
        RecordingSequenceProvider provider{std::vector<provider::Response>{
            provider::Response{
                .blocks = {core::ToolUseContent{.id = "slow-1", .name = "FileRead", .input_json = R"({"value":"x"})"}},
                .stop_reason = core::StopReason::tool_use,
                .usage = {},
                .model_used = std::nullopt,
            },
            provider::Response{
                .blocks = {core::TextContent{.text = "unreached"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = {},
                .model_used = std::string{"fake-1"},
            },
        }};
        agent::Loop loop{provider, default_route()};

        tool::Registry registry;
        // Cancel-aware tool that sleeps well past the supplied per-call timeout.
        auto slow = [](std::string_view, tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
          auto slept = co_await async::sleep_for(ctx.executor, 400ms);
          if (!slept) {
            co_return std::unexpected(slept.error());
          }
          co_return tool::Output::text_only("slow-done");
        };
        REQUIRE(registry.add(canned_tool_def("FileRead"), slow).has_value());

        auto rules = allow_all_rules();
        permission::RecordingAuditSink audit;
        auto ctx = dispatch_context(io, rules, audit);

        // A caller-supplied scheduler with an aggressive 40 ms per-call timeout.
        // If the loop honors it the slow tool times out and the turn fails; the
        // loop's own 60 s default fallback would instead let the tool finish and
        // re-enter the provider for a terminal response.
        agent::ToolScheduler scheduler{io.get_executor(),
                                       registry,
                                       agent::ToolSchedulerOptions{.max_parallel_tools = 4, .per_call_timeout = 40ms}};

        const auto catalog = registry.catalog();
        const std::vector<core::Message> tail{core::Message::user_text("read")};
        auto inputs = base_inputs(catalog, tail);
        inputs.tools = &registry;
        inputs.dispatch_context = &ctx;
        inputs.scheduler = &scheduler;

        auto result = co_await loop.run_turn(inputs);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().kind() == core::ErrorKind::cancelled);
        // Only the first (tool_use) request was sent: the timed-out tool ended
        // the turn instead of producing a tool result that re-enters the provider.
        REQUIRE(provider.requests().size() == 1);
      },
      3s);
}

TEST_CASE("Loop rejects a missing trace executor before calling the provider",
          "[unit][agent][loop][trace][execution]") {
  test::run_async([](asio::io_context&) -> async::Awaitable<void> {
    storage::Pool pool;
    storage::TraceRepository trace{pool};
    RecordingProvider provider{provider::Response{
        .blocks = {core::TextContent{.text = "done"}},
        .stop_reason = core::StopReason::end_turn,
        .model_used = std::nullopt,
    }};
    agent::Loop loop{provider, default_route()};
    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("hello")};
    auto inputs = base_inputs(catalog, tail);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };

    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().kind() == core::ErrorKind::invalid_argument);
    CHECK(result.error().message() == "trace blocking executor is not configured");
    CHECK(provider.calls() == 0);
  });
}

TEST_CASE("Loop persists one terminal trace row for a text turn", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-text"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "done"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = provider::Usage{.input_tokens = 11,
                                         .output_tokens = 5,
                                         .cache_creation_tokens = 2,
                                         .cache_read_tokens = 7,
                                         .cost_estimate = 0.012},
                .model_used = std::string{"fake-trace-model"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = {},
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace me")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .parent_turn_id = turn_id_with(0x22),
        .agent_key = "coder",
        .origin = "cli",
        .context_json = R"json({"source":"test"})json",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "done");

    auto row = co_await trace.get_turn(turn_id_with(0x31));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x31));
    REQUIRE((*row)->parent_turn_id.has_value());
    REQUIRE(*(*row)->parent_turn_id == turn_id_with(0x22));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->agent_key == "coder");
    REQUIRE((*row)->origin == "cli");
    REQUIRE((*row)->route_profile == "fake");
    REQUIRE((*row)->route_model == "fake-trace-model");
    REQUIRE((*row)->started_at_ns > 0);
    REQUIRE((*row)->finished_at_ns >= (*row)->started_at_ns);
    REQUIRE((*row)->stop_reason == "end_turn");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->prompt_prefix_hash == result->rendered_prompt.prefix_hash);
    REQUIRE((*row)->prompt_prefix_bytes == static_cast<std::int64_t>(result->rendered_prompt.prefix_bytes));
    REQUIRE((*row)->active_catalog_hash == result->rendered_prompt.tool_catalog_hash);
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 7);
    REQUIRE((*row)->input_tokens == 11);
    REQUIRE((*row)->output_tokens == 5);
    REQUIRE((*row)->cost_estimate_usd == 0.012);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
    REQUIRE((*row)->context_json == R"json({"source":"test"})json");
  });
}

TEST_CASE("Loop persists a terminal trace row and correlates storage audit rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-audit"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    storage::AuditRepository audit_repo{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = R"({"value":"a"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 11,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 2,
                                     .cache_read_tokens = 7,
                                     .cost_estimate = 0.010},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = provider::Usage{.input_tokens = 3,
                                     .output_tokens = 5,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 1,
                                     .cost_estimate = 0.002},
            .model_used = std::string{"fake-trace-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read-ok");
    auto rules = allow_all_rules();
    permission::StorageAuditSink audit{audit_repo, io.get_executor()};
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .parent_turn_id = turn_id_with(0x22),
        .agent_key = "coder",
        .origin = "cli",
        .context_json = R"json({"source":"test"})json",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");
    REQUIRE(result->iterations == 2);

    auto row = co_await trace.get_turn(turn_id_with(0x31));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x31));
    REQUIRE((*row)->parent_turn_id.has_value());
    REQUIRE(*(*row)->parent_turn_id == turn_id_with(0x22));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->agent_key == "coder");
    REQUIRE((*row)->origin == "cli");
    REQUIRE((*row)->route_profile == "fake");
    REQUIRE((*row)->route_model == "fake-trace-model");
    REQUIRE((*row)->started_at_ns > 0);
    REQUIRE((*row)->finished_at_ns >= (*row)->started_at_ns);
    REQUIRE((*row)->stop_reason == "end_turn");
    REQUIRE((*row)->iteration_count == 2);
    REQUIRE((*row)->prompt_prefix_hash == result->rendered_prompt.prefix_hash);
    REQUIRE((*row)->prompt_prefix_bytes == static_cast<std::int64_t>(result->rendered_prompt.prefix_bytes));
    REQUIRE((*row)->active_catalog_hash == result->rendered_prompt.tool_catalog_hash);
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 8);
    REQUIRE((*row)->input_tokens == 14);
    REQUIRE((*row)->output_tokens == 6);
    REQUIRE((*row)->cost_estimate_usd == 0.012);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
    REQUIRE((*row)->context_json == R"json({"source":"test"})json");

    auto events = co_await audit_repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].tool_name == "FileRead");
    REQUIRE((*events)[0].parent_turn_id.has_value());
    REQUIRE(*(*events)[0].parent_turn_id == (*row)->turn_id);
  });
}

TEST_CASE("Loop generates trace turn ids and correlates storage audit rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-generated-id"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    storage::AuditRepository audit_repo{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = R"({"value":"a"})"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 5,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"fake-tool-model"},
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = provider::Usage{.input_tokens = 3,
                                     .output_tokens = 2,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"fake-final-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read-ok");
    auto rules = allow_all_rules();
    permission::StorageAuditSink audit{audit_repo, io.get_executor()};
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("generate trace id")};
    auto inputs = base_inputs(catalog, tail);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");

    auto rows = co_await trace.list_turns(storage::ListTraceTurnsOptions{.limit = 10});
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 1);
    const auto& row = (*rows)[0];
    REQUIRE_FALSE(core::is_zero_turn_id(row.turn_id));
    REQUIRE(row.stop_reason == "end_turn");
    REQUIRE(row.route_model == "fake-final-model");
    REQUIRE(row.input_tokens == 8);
    REQUIRE(row.output_tokens == 3);

    auto events = co_await audit_repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].parent_turn_id.has_value());
    REQUIRE(*(*events)[0].parent_turn_id == row.turn_id);
  });
}

TEST_CASE("Loop disables trace rows and audit parent ids when trace is off", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-disabled"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    storage::AuditRepository audit_repo{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{
                .id = "t1",
                .name = "FileRead",
                .input_json = R"({"value":"a"})",
            }},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "final"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::string{"fake-trace-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read-ok");
    auto rules = allow_all_rules();
    permission::StorageAuditSink audit{audit_repo, io.get_executor()};
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("trace disabled read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x31);
    inputs.trace = agent::TraceContext{
        .enabled = false,
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(result->text == "final");

    auto turns = co_await trace.list_turns({});
    REQUIRE(turns.has_value());
    REQUIRE(turns->empty());

    auto events = co_await audit_repo.list_events(storage::ListAuditEventsOptions{.scope_key = "scope-A", .limit = 10});
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].tool_name == "FileRead");
    REQUIRE_FALSE((*events)[0].parent_turn_id.has_value());
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop persists provider cancellation trace rows", "[unit][agent][loop][trace][cancellation]") {
  TempDb db{"oran-agent-loop-trace-provider-cancel"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    asio::cancellation_signal signal;
    std::vector<provider::ScriptedTurn> plan;
    plan.push_back(provider::ScriptedTurn{
        .response =
            provider::Response{
                .blocks = {core::TextContent{.text = "late"}},
                .stop_reason = core::StopReason::end_turn,
                .usage = {},
                .model_used = std::string{"never-used"},
            },
        .deltas = {},
        .error = std::nullopt,
        .latency = 1s,
    });
    provider::FakeProvider fake{std::move(plan)};
    agent::Loop loop{fake, default_route()};

    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("cancel provider trace")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x41);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };

    std::optional<core::Result<agent::RunTurnResult>> result;
    std::exception_ptr failure;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
        asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
          failure = ep;
          result = std::move(r);
        }));

    asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
    while (!result.has_value() && !failure) {
      auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
      REQUIRE(tick.has_value());
    }
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(contains_context(result->error(), "cancellation_phase", "provider_initial"));

    auto row = co_await trace.get_turn(turn_id_with(0x41));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x41));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_model == "fake-1");
    REQUIRE((*row)->stop_reason == "cancelled");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 0);
    REQUIRE((*row)->output_tokens == 0);
    REQUIRE((*row)->cancellation_phase.has_value());
    REQUIRE(*(*row)->cancellation_phase == "provider_initial");
  });
}

TEST_CASE("Loop persists tool cancellation trace rows", "[unit][agent][loop][trace][cancellation]") {
  TempDb db{"oran-agent-loop-trace-tool-cancel"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    asio::cancellation_signal signal;
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 9,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 2,
                                     .cache_read_tokens = 3,
                                     .cost_estimate = 0.004},
            .model_used = std::string{"fake-tool-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    auto handler = [](std::string_view, tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
      auto slept = co_await async::sleep_for(ctx.executor, 1s);
      if (!slept) {
        co_return std::unexpected(std::move(slept).error());
      }
      co_return tool::Output::text_only("late");
    };
    REQUIRE(registry.add(canned_tool_def("FileRead"), std::move(handler)).has_value());
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("cancel tool trace")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x42);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x80),
        .agent_key = "coder",
        .origin = "cli",
    };
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;

    std::optional<core::Result<agent::RunTurnResult>> result;
    std::exception_ptr failure;
    asio::co_spawn(
        io,
        [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
        asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
          failure = ep;
          result = std::move(r);
        }));

    asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
    while (!result.has_value() && !failure) {
      auto tick = co_await async::sleep_for(io.get_executor(), 1ms);
      REQUIRE(tick.has_value());
    }
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->has_value());
    REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(contains_context(result->error(), "cancellation_phase", "tools"));

    auto row = co_await trace.get_turn(turn_id_with(0x42));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x42));
    REQUIRE((*row)->session_id == turn_id_with(0x80));
    REQUIRE((*row)->route_model == "fake-tool-model");
    REQUIRE((*row)->stop_reason == "cancelled");
    REQUIRE((*row)->iteration_count == 1);
    REQUIRE((*row)->input_tokens == 9);
    REQUIRE((*row)->output_tokens == 1);
    REQUIRE((*row)->cache_creation_tokens == 2);
    REQUIRE((*row)->cache_read_tokens == 3);
    REQUIRE((*row)->cost_estimate_usd == 0.004);
    REQUIRE((*row)->cancellation_phase.has_value());
    REQUIRE(*(*row)->cancellation_phase == "tools");
    REQUIRE(audit.events().size() == 1);
  });
}

TEST_CASE("Loop preserves multiple tool_results in tool_use order", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks =
                {
                    core::ToolUseContent{.id = "b", .name = "SecondTool", .input_json = R"({"value":"2"})"},
                    core::ToolUseContent{.id = "a", .name = "FirstTool", .input_json = R"({"value":"1"})"},
                },
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "ordered"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FirstTool"), "first");
    add_canned_tool(registry, canned_tool_def("SecondTool"), "second");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);
    ctx.parent_turn_id = turn_id_with(0x70);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("run both")};
    auto inputs = base_inputs(catalog, tail);
    const std::vector<std::string> selected{"FirstTool", "SecondTool"};
    inputs.active_tools = selected;
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests()[1].messages.size() == 3);
    const auto& tool_message = provider.requests()[1].messages[2];
    REQUIRE(tool_message.role == core::Role::tool);
    REQUIRE(tool_message.blocks.size() == 2);
    REQUIRE(tool_result_at(tool_message, 0).tool_use_id == "b");
    REQUIRE(tool_result_at(tool_message, 0).output == R"(second:{"value":"2"})");
    REQUIRE(tool_result_at(tool_message, 1).tool_use_id == "a");
    REQUIRE(tool_result_at(tool_message, 1).output == R"(first:{"value":"1"})");
    REQUIRE(audit.events().size() == 2);
    REQUIRE(audit.events()[0].tool_name == "SecondTool");
    REQUIRE(audit.events()[1].tool_name == "FirstTool");
    REQUIRE_FALSE(audit.events()[0].parent_turn_id.has_value());
    REQUIRE_FALSE(audit.events()[1].parent_turn_id.has_value());
    REQUIRE(ctx.parent_turn_id.has_value());
    REQUIRE(*ctx.parent_turn_id == turn_id_with(0x70));
  });
}

TEST_CASE("Loop returns model-visible tool errors as tool_result blocks", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "missing", .name = "ToolMissing", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::TextContent{.text = "repaired"}},
            .stop_reason = core::StopReason::end_turn,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("missing")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE(result.has_value());
    REQUIRE(provider.requests().size() == 2);
    const auto& tool_result = tool_result_at(provider.requests()[1].messages[2], 0);
    REQUIRE(tool_result.tool_use_id == "missing");
    REQUIRE(tool_result.is_error);
    REQUIRE(tool_result.output.contains("tool error: tool is not registered"));
    REQUIRE(tool_result.output.contains("tool: ToolMissing"));
  });
}

TEST_CASE("Loop propagates infrastructure errors from tool dispatch", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "audit broke", false, core::ErrorKind::internal);
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("read")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(result.error().message() == "audit broke");
  });
}

TEST_CASE("Loop annotates cancellation during tool dispatch", "[unit][agent][loop][cancellation]") {
  asio::io_context io;
  asio::cancellation_signal signal;

  RecordingSequenceProvider provider{std::vector<provider::Response>{
      provider::Response{
          .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"}},
          .stop_reason = core::StopReason::tool_use,
          .usage = {},
          .model_used = std::nullopt,
      },
  }};
  agent::Loop loop{provider, default_route()};
  tool::Registry registry;
  auto handler = [](std::string_view, tool::DispatchContext& ctx) -> async::Awaitable<core::Result<tool::Output>> {
    auto slept = co_await async::sleep_for(ctx.executor, 1s);
    if (!slept) {
      co_return std::unexpected(std::move(slept).error());
    }
    co_return tool::Output::text_only("late");
  };
  REQUIRE(registry.add(canned_tool_def("FileRead"), std::move(handler)).has_value());
  auto rules = allow_all_rules();
  permission::RecordingAuditSink audit;
  auto ctx = dispatch_context(io, rules, audit);

  const auto catalog = registry.catalog();
  const std::vector<core::Message> tail{core::Message::user_text("cancel tool")};
  auto inputs = base_inputs(catalog, tail);
  inputs.tools = &registry;
  inputs.dispatch_context = &ctx;
  std::optional<core::Result<agent::RunTurnResult>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<core::Result<agent::RunTurnResult>> { co_return co_await loop.run_turn(inputs); },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<agent::RunTurnResult> r) {
        failure = ep;
        result = std::move(r);
        io.stop();
      }));

  asio::post(io, [&] { signal.emit(asio::cancellation_type::terminal); });
  io.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE(contains_context(result->error(), "reason", "parent_cancelled"));
  REQUIRE(contains_context(result->error(), "cancellation_phase", "tools"));
  REQUIRE(provider.requests().size() == 1);
  REQUIRE(audit.events().size() == 1);
}

TEST_CASE("Loop stops repeated tool_use turns at the iteration cap", "[unit][agent][loop]") {
  test::run_async([](asio::io_context& io) -> async::Awaitable<void> {
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t2", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = {},
            .model_used = std::nullopt,
        },
    }};
    agent::Loop loop{provider, default_route(), agent::LoopOptions{.max_iterations = 2}};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("loop")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(contains_context(result.error(), "reason", "iteration_cap"));
    REQUIRE(contains_context(result.error(), "max_iterations", "2"));
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(audit.events().size() == 2);
  });
}

TEST_CASE("Loop records usage and terminal failure for rejected tool IDs", "[unit][agent][loop][trace][tool-identity]") {
  TempDb db{"oran-agent-loop-tool-id-trace"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());
    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "same", .name = "Count", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 4, .output_tokens = 1, .cost_estimate = std::nullopt},
            .model_used = std::string{"first-model"},
        },
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "same", .name = "Count", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 6, .output_tokens = 2, .cost_estimate = std::nullopt},
            .model_used = std::string{"last-model"},
        },
    }};
    agent::Loop loop{provider, default_route()};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("Count"), "effect");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto context = dispatch_context(io, rules, audit);
    const auto catalog = registry.catalog();
    const auto tail = std::vector{core::Message::user_text("perform the operation")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &context;
    inputs.turn_id = turn_id_with(0x56);
    inputs.trace = agent::TraceContext{.repository = &trace,
                                       .blocking_executor = io.get_executor(),
                                       .session_id = turn_id_with(0x90),
                                       .agent_key = "coder",
                                       .origin = "cli"};
    auto result = co_await loop.run_turn(inputs);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::upstream);
    REQUIRE(contains_context(result.error(), "reason", "duplicate_tool_use_id"));
    REQUIRE(audit.events().size() == 1);
    auto row = co_await trace.get_turn(*inputs.turn_id);
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    CHECK((*row)->stop_reason == "error");
    CHECK((*row)->iteration_count == 2);
    CHECK((*row)->route_model == "last-model");
    CHECK((*row)->input_tokens == 10);
    CHECK((*row)->output_tokens == 3);
  });
}

TEST_CASE("Loop persists iteration-cap trace rows", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-trace-iteration-cap"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());

    RecordingSequenceProvider provider{std::vector<provider::Response>{
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t1", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 4,
                                     .output_tokens = 1,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::nullopt,
        },
        provider::Response{
            .blocks = {core::ToolUseContent{.id = "t2", .name = "FileRead", .input_json = "{}"}},
            .stop_reason = core::StopReason::tool_use,
            .usage = provider::Usage{.input_tokens = 6,
                                     .output_tokens = 2,
                                     .cache_creation_tokens = 0,
                                     .cache_read_tokens = 0,
                                     .cost_estimate = std::nullopt},
            .model_used = std::string{"iteration-final"},
        },
    }};
    agent::Loop loop{provider, default_route(), agent::LoopOptions{.max_iterations = 2}};
    tool::Registry registry;
    add_canned_tool(registry, canned_tool_def("FileRead"), "read");
    auto rules = allow_all_rules();
    permission::RecordingAuditSink audit;
    auto ctx = dispatch_context(io, rules, audit);

    const auto catalog = registry.catalog();
    const std::vector<core::Message> tail{core::Message::user_text("cap trace")};
    auto inputs = base_inputs(catalog, tail);
    inputs.tools = &registry;
    inputs.dispatch_context = &ctx;
    inputs.turn_id = turn_id_with(0x55);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x90),
        .agent_key = "coder",
        .origin = "cli",
    };
    auto result = co_await loop.run_turn(inputs);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::internal);
    REQUIRE(contains_context(result.error(), "reason", "iteration_cap"));
    REQUIRE(contains_context(result.error(), "max_iterations", "2"));

    auto row = co_await trace.get_turn(turn_id_with(0x55));
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    REQUIRE((*row)->turn_id == turn_id_with(0x55));
    REQUIRE((*row)->session_id == turn_id_with(0x90));
    REQUIRE((*row)->stop_reason == "error");
    REQUIRE((*row)->iteration_count == 2);
    REQUIRE((*row)->route_model == "iteration-final");
    REQUIRE(provider.requests().size() == 2);
    REQUIRE(provider.requests().front().cache.has_value());
    CHECK((*row)->prompt_prefix_hash == provider.requests().front().cache->prefix_hash);
    CHECK((*row)->prompt_prefix_bytes == static_cast<std::int64_t>(provider.requests().front().cache->prefix_bytes));
    REQUIRE((*row)->input_tokens == 10);
    REQUIRE((*row)->output_tokens == 3);
    REQUIRE_FALSE((*row)->cancellation_phase.has_value());
  });
}

TEST_CASE("Loop with no iterations makes no provider request or trace row", "[unit][agent][loop][trace]") {
  TempDb db{"oran-agent-loop-zero-iterations"};
  test::run_async([&db](asio::io_context& io) -> async::Awaitable<void> {
    auto pool = open_trace_pool(io, db);
    storage::TraceRepository trace{pool};
    const auto migrated = co_await trace.migrate();
    REQUIRE(migrated.has_value());
    RecordingSequenceProvider provider{std::vector<provider::Response>{}};
    agent::Loop loop{provider, default_route(), agent::LoopOptions{.max_iterations = 0}};
    const auto catalog = loop_catalog();
    const std::vector<core::Message> tail{core::Message::user_text("no attempts")};
    auto inputs = base_inputs(catalog, tail);
    inputs.turn_id = turn_id_with(0x56);
    inputs.trace = agent::TraceContext{
        .repository = &trace,
        .blocking_executor = io.get_executor(),
        .session_id = turn_id_with(0x90),
        .agent_key = "coder",
        .origin = "test",
    };
    const auto result = co_await loop.run_turn(inputs);
    REQUIRE_FALSE(result.has_value());
    CHECK(contains_context(result.error(), "reason", "iteration_cap"));
    CHECK(provider.requests().empty());
    const auto row = co_await trace.get_turn(*inputs.turn_id);
    REQUIRE(row.has_value());
    CHECK_FALSE(row->has_value());
  });
}
