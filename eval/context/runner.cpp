#include "evaluation.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <set>

#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <oran/agent/scheduler.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/runtime_assembly.hpp>
#include <oran/config/config.hpp>
#include <oran/core/error.hpp>
#include <oran/hook/bus.hpp>
#include <oran/io/private_directory.hpp>
#include <oran/memory/session.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::evaluation {
namespace {
using nlohmann::json;

struct Budget {
  std::size_t used{};
  std::size_t limit{};
};

class MeasuredProvider final : public provider::System {
public:
  MeasuredProvider(provider::System& backend, Budget& budget) : backend_{backend}, budget_{budget} {}
  mutable std::size_t attempts{};
  mutable json calls = json::array();
  async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink) const override {
    if (budget_.used >= budget_.limit)
      co_return std::unexpected(core::Error::invalid_argument("evaluation provider call budget exhausted"));
    ++budget_.used;
    ++attempts;
    const bool summary = !request.cache.has_value();
    auto response = co_await backend_.send(std::move(request), std::move(target), sink);
    json call = {{"kind", summary ? "summary" : "answer"}};
    if (response) {
      std::size_t text_bytes = 0, thinking_bytes = 0;
      for (const auto& block : response->blocks) {
        if (const auto* text = std::get_if<core::TextContent>(&block))
          text_bytes += text->text.size();
        if (const auto* thinking = std::get_if<core::ThinkingContent>(&block))
          thinking_bytes += thinking->thinking.size();
      }
      call["stop_reason"] = core::enum_name(response->stop_reason);
      call["text_bytes"] = text_bytes;
      call["thinking_bytes"] = thinking_bytes;
      call["output_tokens"] = response->usage.output_tokens;
    } else
      call["error_kind"] = core::enum_name(response.error().kind());
    calls.push_back(std::move(call));
    co_return response;
  }

private:
  provider::System& backend_;
  Budget& budget_;
};

// This provider checks runner plumbing only. It deliberately knows fixture truth;
// reports identify its results as controlled, never deployment-model evidence.
class ControlledProvider final : public provider::System {
public:
  explicit ControlledProvider(const Scenario& scenario) : scenario_{scenario} {}
  async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget, provider::EventSink*) const override {
    provider::Response result;
    result.model_used = "controlled";
    result.usage.input_tokens = 100;
    result.usage.output_tokens = 50;
    if (request.system_prompt.value_or("").starts_with("Produce a bounded session handoff")) {
      result.blocks.emplace_back(core::TextContent{
          .text = "Objective:\n" + scenario_.expected.dump() +
                  "\nConstraints:\nKeep all task constraints.\nDecisions:\nUse current decision.\nCompleted:\nOnly "
                  "observed work.\nPending:\nTests and release.\nReferences:\nFixture transcript.\n"});
    } else if (scenario_.large_tool && (request.messages.empty() || request.messages.back().role != core::Role::tool)) {
      result.stop_reason = core::StopReason::tool_use;
      result.blocks.emplace_back(core::ToolUseContent{.id = "fixture-read", .name = "FixtureRead", .input_json = "{}"});
    } else
      result.blocks.emplace_back(core::TextContent{.text = scenario_.expected.dump()});
    co_return result;
  }

private:
  const Scenario& scenario_;
};

async::Awaitable<core::Result<std::unique_ptr<bootstrap::RuntimeAssembly>>>
open_assembly(std::string path, asio::any_io_executor worker) {
  auto directory = io::PrivateDirectory::open(path);
  if (!directory)
    co_return std::unexpected(directory.error());
  auto built = bootstrap::RuntimeAssembly::build(
      path,
      worker,
      {.audit_enabled = true, .trace_enabled = true, .session_memory_enabled = true, .longterm_memory_enabled = false});
  if (!built)
    co_return std::unexpected(built.error());
  co_return std::make_unique<bootstrap::RuntimeAssembly>(std::move(*built));
}

std::vector<core::Message> later_pressure() {
  std::vector<core::Message> messages;
  for (int i = 0; i < 24; ++i) {
    messages.push_back(core::Message::user_text(std::format(
        "Additional fixture {} inspection: delimiter diagnostics include empty trailing cells, escaped quotes, "
        "Unicode labels and multiline records. Record this observation; retain all existing task state. "
        "No build, test, release or implementation decision occurs in this observation.",
        i)));
    messages.push_back(
        core::Message::assistant_text("Inspection recorded. These sample rows do not constitute test execution, "
                                      "implementation completion or release. "
                                      "The active goal, constraints, decision and pending work continue unchanged."));
  }
  return messages;
}

async::Awaitable<core::Result<json>> phase(const Scenario& scenario,
                                           const Options& options,
                                           std::string path,
                                           bool compacted,
                                           int phase_index,
                                           core::TurnId session_id,
                                           provider::System& backend,
                                           const provider::Route& route,
                                           asio::any_io_executor coordinator,
                                           asio::any_io_executor worker,
                                           Budget& budget) {
  const auto started = std::chrono::steady_clock::now();
  auto assembly_result = co_await asio::co_spawn(worker, open_assembly(path, worker), asio::use_awaitable);
  if (!assembly_result)
    co_return std::unexpected(assembly_result.error());
  auto& assembly = **assembly_result;
  const memory::session::SessionId id{core::format_turn_id_hex(session_id)};
  const memory::session::AgentKey agent{"context-eval"};
  auto seeded = phase_index == 0 ? scenario.history : later_pressure();
  auto appended =
      co_await asio::co_spawn(worker, assembly.session_store()->append_all(id, agent, seeded), asio::use_awaitable);
  if (!appended)
    co_return std::unexpected(appended.error());
  auto before = co_await asio::co_spawn(worker, assembly.session_store()->load_context(id, agent), asio::use_awaitable);
  if (!before)
    co_return std::unexpected(before.error());

  hook::ProviderUsage usage;
  std::size_t responses = 0;
  std::size_t provider_errors = 0;
  std::set<std::string> models;
  bool all_costs_known = true;
  hook::Sink observer{.id = "eval-metrics",
                      .observe = [&](hook::Event, hook::PayloadPtr payload) -> async::Awaitable<void> {
                        if (const auto* event = std::get_if<hook::ProviderResponsePayload>(payload.get())) {
                          ++responses;
                          usage.input_tokens += event->usage.input_tokens;
                          usage.output_tokens += event->usage.output_tokens;
                          usage.cache_read_tokens += event->usage.cache_read_tokens;
                          usage.cache_creation_tokens += event->usage.cache_creation_tokens;
                          all_costs_known = all_costs_known && event->usage.cost_estimate.has_value();
                          usage.cost_estimate =
                              usage.cost_estimate.value_or(0) + event->usage.cost_estimate.value_or(0);
                          models.insert(event->served_model);
                        } else if (std::holds_alternative<hook::ProviderErrorPayload>(*payload))
                          ++provider_errors;
                        co_return;
                      }};
  assembly.hook_bus().subscribe(observer, {hook::Event::provider_response, hook::Event::provider_error});

  auto config_result = config::Config::parse(
      scenario.large_tool
          ? R"({"runtime":{"prompt":{"active_tools":["FixtureRead"]}},"permissions":{"allow":[{"tool_pattern":"FixtureRead"}]}})"
          : R"({"runtime":{"prompt":{"active_tools":[]}}})");
  if (!config_result)
    co_return std::unexpected(config_result.error());
  tool::Registry registry;
  std::size_t tool_calls = 0;
  if (scenario.large_tool) {
    auto added = registry.add(
        core::ToolDef::with_no_input("FixtureRead", "Read the fixed evaluation diagnostic fixture."),
        [&](std::string_view, tool::DispatchContext&) -> async::Awaitable<core::Result<tool::Output>> {
          ++tool_calls;
          std::string output =
              "Fixture marker: fixture_schema_v2. This is diagnostic data, not completed regression tests.\n";
          for (int i = 0; i < 70; ++i)
            output += std::format(
                "row {}: quoted field, escaped delimiter, Unicode label; inspection only, no state change.\n",
                i);
          co_return tool::Output::text_only(std::move(output));
        });
    if (!added)
      co_return std::unexpected(added.error());
  }
  agent::ToolScheduler scheduler{coordinator, registry};
  MeasuredProvider measured{backend, budget};
  bootstrap::AgentSessionOptions session_options;
  session_options.executor = coordinator;
  session_options.blocking_executor = worker;
  session_options.assembly = &assembly;
  session_options.config = &*config_result;
  session_options.provider = &measured;
  session_options.route = route;
  session_options.session_id = session_id;
  session_options.agent_key = agent.value;
  session_options.scope_key = id.value;
  session_options.origin = "context-evaluation";
  session_options.registry = &registry;
  session_options.scheduler = &scheduler;
  session_options.max_child_runs = 0;
  session_options.system_preamble =
      "Continue the user's task accurately from the supplied session context. "
      "Current corrections override older decisions. Distinguish observed results from plans. "
      "Return only the requested JSON object, using the exact task labels from the conversation.";
  session_options.context.max_tokens = compacted ? options.context_tokens : 262144;
  session_options.context.summary_max_bytes = 2048;
  session_options.max_tokens = 1024;
  if (!scenario.large_tool)
    session_options.tool_choice.reset();
  session_options.stream = false;
  auto session = bootstrap::AgentSession::create(std::move(session_options));
  if (!session)
    co_return std::unexpected(session.error());
  const auto prompt =
      std::string{scenario.large_tool ? "Call FixtureRead now, then " : "Please "} +
      "give the current handoff as JSON: goal (string), constraints (array), decision (string), completed (array), "
      "pending (array)" +
      (scenario.large_tool ? ", tool_marker (the marker from the tool)" : "") +
      ". Use the exact labels from our task, without explaining or restating superseded decisions.";
  auto answer = co_await (*session)->run_prompt({.prompt = prompt});
  // AgentSession has joined every tool before returning. A cancelled coroutine
  // still records its checkpoint and metrics after the join.
  if (!answer && answer.error().kind() == core::ErrorKind::cancelled)
    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
  session->reset();
  auto after = co_await asio::co_spawn(worker, assembly.session_store()->load_context(id, agent), asio::use_awaitable);
  if (!after)
    co_return std::unexpected(after.error());
  const bool exercised = compacted ? (phase_index == 0 ? after->checkpoint.revision >= 2
                                                       : after->checkpoint.revision > before->checkpoint.revision)
                                   : after->checkpoint.revision == 0;
  auto scored = answer ? grade(answer->text, scenario) : json{{"passed", false}};
  const bool tool_exercised = !scenario.large_tool || tool_calls > 0;
  auto result = json{
      {"phase", phase_index == 0 ? "after_pressure" : "after_reopen"},
      {"passed", scored.value("passed", false) && exercised && tool_exercised},
      {"grading", scored},
      {"compaction_exercised", exercised},
      {"tool_calls", tool_calls},
      {"checkpoint_before", before->checkpoint.revision},
      {"checkpoint_after", after->checkpoint.revision},
      {"covered_sequence", after->checkpoint.covered_sequence},
      {"stored_messages", after->message_count},
      {"attempts", measured.attempts},
      {"calls", measured.calls},
      {"successful_responses", responses},
      {"provider_errors", provider_errors},
      {"models", models},
      {"input_tokens", usage.input_tokens},
      {"output_tokens", usage.output_tokens},
      {"cache_read_tokens", usage.cache_read_tokens},
      {"cache_creation_tokens", usage.cache_creation_tokens},
      {"cost_estimate_usd", all_costs_known && responses > 0 ? json(usage.cost_estimate.value_or(0)) : json(nullptr)},
      {"elapsed_ms",
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()}};
  if (!answer) {
    result["error_kind"] = core::enum_name(answer.error().kind());
    for (const auto& [key, value] : answer.error().context()) {
      if (key == "reason" && value == "context_budget")
        result["context_error"] = answer.error().message();
      if (key != "http_status")
        continue;
      int status{};
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), status);
      if (error == std::errc{} && end == value.data() + value.size() && status >= 100 && status <= 599)
        result["http_status"] = status;
    }
  }
  co_return result;
}
}  // namespace

async::Awaitable<core::Result<void>> run(const Options& options,
                                         provider::System* backend,
                                         provider::Route route,
                                         asio::any_io_executor coordinator,
                                         asio::any_io_executor worker,
                                         json& report) {
  if (auto valid = validate(options); !valid)
    co_return std::unexpected(valid.error());
  report = {{"mode", backend ? "live" : "controlled_plumbing_check"},
            {"complete", false},
            {"passed", false},
            {"fixture", "synthetic_task_handoffs"},
            {"context_tokens", options.context_tokens},
            {"baseline_context_tokens", 262144},
            {"summary_max_bytes", 2048},
            {"repeats", options.repeats},
            {"max_calls", options.max_calls},
            {"cases", json::array()}};
  Budget budget{.limit = options.max_calls};
  bool passed = true;
  for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
    for (const auto& scenario : scenarios()) {
      if (options.selected_case != "all" && scenario.name != options.selected_case)
        continue;
      ControlledProvider controlled{scenario};
      for (const bool compacted : {false, true}) {
        if (budget.used >= budget.limit)
          co_return std::unexpected(core::Error::invalid_argument("evaluation call budget exhausted"));
        auto identity = core::generate_turn_id();
        if (!identity)
          co_return std::unexpected(identity.error());
        const auto label = std::format("{}-{}-{}", scenario.name, repeat + 1, compacted ? "compacted" : "baseline");
        const auto path = (std::filesystem::path{options.output} / label).string();
        json row = {{"scenario", scenario.name},
                    {"repeat", repeat + 1},
                    {"variant", compacted ? "compacted" : "baseline"},
                    {"session_id", core::format_turn_id_hex(*identity)},
                    {"expected", scenario.expected},
                    {"phases", json::array()}};
        for (int index = 0; index < 2; ++index) {
          auto result = co_await phase(scenario,
                                       options,
                                       path,
                                       compacted,
                                       index,
                                       *identity,
                                       backend ? *backend : controlled,
                                       route,
                                       coordinator,
                                       worker,
                                       budget);
          report["attempts"] = budget.used;
          if (!result) {
            row["phases"].push_back({{"passed", false}, {"error_kind", core::enum_name(result.error().kind())}});
            report["cases"].push_back(std::move(row));
            co_return std::unexpected(result.error());
          }
          passed = passed && result->value("passed", false);
          const bool cancelled = result->value("error_kind", std::string{}) == "cancelled";
          const auto status = result->value("http_status", 0);
          const bool unavailable = status == 401 || status == 402 || status == 403;
          row["phases"].push_back(std::move(*result));
          if (unavailable) {
            report["cases"].push_back(std::move(row));
            co_return std::unexpected(core::Error::upstream("evaluation provider account unavailable"));
          }
          if (cancelled) {
            report["cases"].push_back(std::move(row));
            co_return std::unexpected(core::Error::cancelled());
          }
        }
        std::println("{}: {}",
                     label,
                     row["phases"][0]["passed"] == true && row["phases"][1]["passed"] == true ? "pass" : "fail");
        report["cases"].push_back(std::move(row));
      }
    }
  }
  report["complete"] = true;
  report["passed"] = passed;
  co_return core::Result<void>{};
}
}  // namespace orangutan::evaluation
