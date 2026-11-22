#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <oran/async.hpp>
#include <oran/bootstrap.hpp>
#include <oran/config.hpp>
#include <oran/memory.hpp>
#include <oran/provider.hpp>
#include <oran/tool.hpp>

#include "../test-helpers/run_async.hpp"

namespace team_tests {
using namespace orangutan;
using namespace std::chrono_literals;
using Json = nlohmann::json;

inline provider::Response answer(std::string value) {
  return {.blocks = {core::TextContent{std::move(value)}}, .usage = {}, .model_used = std::nullopt};
}
inline provider::Response call(std::string name, Json input, std::string id = "call") {
  return {.blocks = {core::ToolUseContent{.id = std::move(id), .name = std::move(name), .input_json = input.dump()}},
          .stop_reason = core::StopReason::tool_use,
          .usage = {},
          .model_used = std::nullopt};
}
inline std::string message_text(const core::Message& message) {
  std::string text;
  for (const auto& block : message.blocks)
    if (const auto* content = std::get_if<core::TextContent>(&block))
      text += content->text;
  return text;
}
inline Json assignment(const provider::Request& request) {
  const auto found = std::ranges::find_last_if(request.messages, [](const auto& message) {
    return message_text(message).starts_with("[Team assignment");
  });
  if (found.empty())
    return Json::object();
  const auto text = message_text(found.front());
  return Json::parse(text.substr(text.find('{')));
}
inline bool has_tool_result(const provider::Request& request, std::string_view id) {
  return std::ranges::any_of(request.messages, [id](const auto& message) {
    return std::ranges::any_of(message.blocks, [id](const auto& block) {
      const auto* result = std::get_if<core::ToolResultContent>(&block);
      return result && result->tool_use_id == id;
    });
  });
}

class Provider final : public provider::System {
public:
  mutable std::vector<provider::Request> requests;
  mutable std::vector<core::ToolResultContent> leader_results;
  mutable std::size_t completions{0};
  std::function<async::Awaitable<core::Result<provider::Response>>(provider::Request, Json)> specialist;

  async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget, provider::EventSink*) const override {
    requests.push_back(request);
    auto task = assignment(request);
    if (!task.empty()) {
      if (specialist)
        co_return co_await specialist(std::move(request), std::move(task));
      co_return answer(task.at("task").get<std::string>() + " report " + std::to_string(task.at("attempt").get<int>()));
    }
    if (request.messages.back().role == core::Role::tool) {
      for (const auto& block : request.messages.back().blocks)
        if (const auto* result = std::get_if<core::ToolResultContent>(&block))
          leader_results.push_back(*result);
      co_return answer("leader continued");
    }
    auto prompt = message_text(request.messages.back());
    if (prompt.starts_with("invoke:")) {
      const auto input = Json::parse(prompt.substr(7));
      co_return call(input.at("name"), input.at("input"));
    }
    if (prompt.starts_with("[Runtime background-task"))
      ++completions;
    co_return answer("leader continued");
  }
};

class Fixture {
public:
  explicit Fixture(asio::any_io_executor executor,
                   bootstrap::BackgroundTaskOptions limits = {.automatic_delivery = true, .enable_teams = true},
                   std::string_view configuration = R"({"permissions":{"allow":[{"tool_pattern":"*"}]}})",
                   bool trace = false)
      : executor{executor} {
    auto id = core::generate_turn_id();
    REQUIRE(id.has_value());
    path = std::filesystem::temp_directory_path() / ("oran-team-" + core::format_turn_id_hex(*id));
    std::filesystem::create_directory(path);
    auto parsed = config::Config::parse(configuration);
    REQUIRE(parsed.has_value());
    config.emplace(std::move(*parsed));
    auto built = bootstrap::RuntimeAssembly::build(
        path.string(),
        executor,
        {.audit_enabled = trace, .trace_enabled = trace, .longterm_memory_enabled = false});
    REQUIRE(built.has_value());
    assembly.emplace(std::move(*built));
    auto service = bootstrap::BackgroundTasks::create(executor, *assembly, *config, limits);
    REQUIRE(service.has_value());
    tasks = std::move(*service);
    options.executor = options.blocking_executor = executor;
    options.assembly = &*assembly;
    options.config = &*config;
    options.provider = &provider;
    options.route.primary.profile = "test";
    options.route.primary.model = "test";
    options.mode = permission::Mode::strict;
    options.session_id = *id;
    options.agent_key = "leader";
    options.scope_key = "team-scope";
    options.background_tasks = tasks.get();
    options.per_agent_overlay = "private parent context";
    auto policy = bootstrap::materialize_permissions(options.mode, config->permissions().rules);
    REQUIRE(policy.has_value());
    rules = std::move(*policy);
    auto created = bootstrap::AgentSession::create(options);
    REQUIRE(created.has_value());
    session = std::move(*created);
  }
  ~Fixture() {
    session.reset();
    tasks.reset();
    assembly.reset();
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  core::Result<tool::Output> command(tool::TeamCommand command,
                                     std::optional<core::TurnId> parent_turn = std::nullopt) {
    if (auto* update = std::get_if<tool::TeamTaskUpdateRequest>(&command); update && update->team_id.empty())
      update->team_id = board().team_id;
    auto context = tool::DispatchContext::for_now(executor,
                                                  rules,
                                                  assembly->audit_sink(),
                                                  options.scope_key,
                                                  options.agent_key,
                                                  options.identity);
    context.mode = options.mode;
    context.parent_turn_id = parent_turn;
    return tasks->team_command(options, std::move(command), context);
  }
  bootstrap::TeamSnapshot board() {
    auto result = tasks->get_team(bootstrap::task_owner(options));
    REQUIRE(result.has_value());
    return std::move(*result);
  }
  bootstrap::TeamTaskSnapshot task(std::string_view key) {
    auto current = board();
    auto found = std::ranges::find(current.tasks, key, &bootstrap::TeamTaskSnapshot::key);
    REQUIRE(found != current.tasks.end());
    return *found;
  }
  async::Awaitable<bootstrap::TeamTaskSnapshot> settled(std::string key) {
    for (;;) {
      auto current = task(key);
      if (current.state == bootstrap::TeamTaskState::in_review || current.state == bootstrap::TeamTaskState::failed ||
          current.state == bootstrap::TeamTaskState::cancelled || current.state == bootstrap::TeamTaskState::accepted)
        co_return current;
      auto pause = co_await async::sleep_for(executor, 1ms);
      REQUIRE(pause.has_value());
    }
  }
  async::Awaitable<core::ToolResultContent> native(std::string name, Json input) {
    auto result = co_await session->run_prompt(
        {.prompt = "invoke:" + Json{{"name", std::move(name)}, {"input", std::move(input)}}.dump()});
    REQUIRE(result.has_value());
    REQUIRE_FALSE(provider.leader_results.empty());
    co_return provider.leader_results.back();
  }
  async::Awaitable<void> shutdown() {
    auto closed = co_await tasks->shutdown();
    REQUIRE(closed.has_value());
  }

  asio::any_io_executor executor;
  std::filesystem::path path;
  std::optional<config::Config> config;
  std::optional<bootstrap::RuntimeAssembly> assembly;
  permission::RuleSet rules;
  Provider provider;
  std::unique_ptr<bootstrap::BackgroundTasks> tasks;
  bootstrap::AgentSessionOptions options;
  std::unique_ptr<bootstrap::AgentSession> session;
};
}  // namespace team_tests
