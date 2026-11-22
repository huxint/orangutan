#include <oran/tool/builtins.hpp>

#include <array>
#include <utility>

#include <nlohmann/json.hpp>
#include <oran/core/enum_names.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {
using Json = nlohmann::json;
using core::Error;
using core::Result;

Result<std::vector<std::string>> strings(const Json& object, std::string_view field, bool required = false) {
  if (!object.contains(field)) {
    if (required)
      return std::unexpected(Error::invalid_argument("missing team array").with("field", std::string{field}));
    return std::vector<std::string>{};
  }
  const auto& values = object[field];
  if (!values.is_array() || values.size() > 32)
    return std::unexpected(Error::invalid_argument("invalid team array").with("field", std::string{field}));
  std::vector<std::string> result;
  for (const auto& value : values) {
    if (!value.is_string())
      return std::unexpected(Error::invalid_argument("team array entries must be strings"));
    result.push_back(value.get<std::string>());
  }
  return result;
}

Result<std::size_t> number(const Json& object, std::string_view field, std::size_t fallback) {
  if (!object.contains(field))
    return fallback;
  const auto& value = object[field];
  if (!value.is_number_unsigned() || value.get<std::uint64_t>() > 65536)
    return std::unexpected(Error::invalid_argument("invalid team integer").with("field", std::string{field}));
  return value.get<std::size_t>();
}

Result<TeamCommand> parse_command(std::string_view name, std::string_view input) {
  constexpr auto create_fields = std::to_array<std::string_view>({"goal", "members"});
  constexpr auto task_fields = std::to_array<std::string_view>({"key", "member", "prompt", "depends_on"});
  constexpr auto update_fields = std::to_array<std::string_view>({"key", "attempt", "action", "feedback", "team_id"});
  constexpr auto get_fields = std::to_array<std::string_view>({"key", "offset", "max_bytes"});
  constexpr auto message_fields = std::to_array<std::string_view>({"recipient", "task_key", "message"});
  constexpr auto member_fields = std::to_array<std::string_view>({"name", "instructions", "tools"});
  const auto fields = name == TEAM_CREATE_NAME        ? std::span<const std::string_view>{create_fields}
                      : name == TEAM_TASK_CREATE_NAME ? std::span<const std::string_view>{task_fields}
                      : name == TEAM_TASK_UPDATE_NAME ? std::span<const std::string_view>{update_fields}
                      : name == TEAM_GET_NAME         ? std::span<const std::string_view>{get_fields}
                      : name == TEAM_MESSAGE_NAME     ? std::span<const std::string_view>{message_fields}
                      : name == TEAM_MEMBER_ADD_NAME  ? std::span<const std::string_view>{member_fields}
                                                      : std::span<const std::string_view>{};
  auto parsed = detail::parse_input_object(input, name, fields);
  if (!parsed)
    return std::unexpected(std::move(parsed).error());
  if (name == TEAM_STOP_NAME && !parsed->empty())
    return std::unexpected(Error::invalid_argument("TeamStop accepts an empty object"));
  if (name == TEAM_MEMBER_ADD_NAME) {
    auto key = detail::require_string_field(*parsed, name, "name");
    auto instructions = detail::require_string_field(*parsed, name, "instructions");
    auto tools = strings(*parsed, "tools", true);
    if (!key)
      return std::unexpected(std::move(key).error());
    if (!instructions)
      return std::unexpected(std::move(instructions).error());
    if (!tools)
      return std::unexpected(std::move(tools).error());
    return TeamMemberAddRequest{{std::move(*key), std::move(*instructions), std::move(*tools)}};
  }
  if (name == TEAM_MESSAGE_NAME) {
    auto recipient = detail::require_string_field(*parsed, name, "recipient");
    auto task = detail::require_string_field(*parsed, name, "task_key");
    auto message = detail::require_string_field(*parsed, name, "message");
    if (!recipient)
      return std::unexpected(std::move(recipient).error());
    if (!task)
      return std::unexpected(std::move(task).error());
    if (!message)
      return std::unexpected(std::move(message).error());
    return TeamMessageRequest{std::move(*recipient), std::move(*task), std::move(*message)};
  }
  if (name == TEAM_CREATE_NAME) {
    auto goal = detail::require_string_field(*parsed, name, "goal");
    if (!goal)
      return std::unexpected(std::move(goal).error());
    if (!parsed->contains("members") || !(*parsed)["members"].is_array() || (*parsed)["members"].size() > 4)
      return std::unexpected(Error::invalid_argument("members must be an array of at most four specialists"));
    TeamCreateRequest request{.goal = std::move(*goal)};
    for (const auto& value : (*parsed)["members"]) {
      auto member = detail::parse_input_object(value.dump(), name, member_fields);
      if (!member)
        return std::unexpected(std::move(member).error());
      auto key = detail::require_string_field(*member, name, "name");
      auto instructions = detail::require_string_field(*member, name, "instructions");
      auto tools = strings(*member, "tools", true);
      if (!key)
        return std::unexpected(std::move(key).error());
      if (!instructions)
        return std::unexpected(std::move(instructions).error());
      if (!tools)
        return std::unexpected(std::move(tools).error());
      request.members.push_back({std::move(*key), std::move(*instructions), std::move(*tools)});
    }
    return request;
  }
  if (name == TEAM_TASK_CREATE_NAME) {
    auto key = detail::require_string_field(*parsed, name, "key");
    auto member = detail::require_string_field(*parsed, name, "member");
    auto prompt = detail::require_string_field(*parsed, name, "prompt");
    auto dependencies = strings(*parsed, "depends_on");
    if (!key)
      return std::unexpected(std::move(key).error());
    if (!member)
      return std::unexpected(std::move(member).error());
    if (!prompt)
      return std::unexpected(std::move(prompt).error());
    if (!dependencies)
      return std::unexpected(std::move(dependencies).error());
    return TeamTaskCreateRequest{std::move(*key), std::move(*member), std::move(*prompt), std::move(*dependencies)};
  }
  if (name == TEAM_TASK_UPDATE_NAME) {
    auto team = detail::require_string_field(*parsed, name, "team_id");
    auto key = detail::require_string_field(*parsed, name, "key");
    auto action = detail::require_string_field(*parsed, name, "action");
    auto attempt = number(*parsed, "attempt", 0);
    if (!team)
      return std::unexpected(std::move(team).error());
    if (!key)
      return std::unexpected(std::move(key).error());
    if (!action)
      return std::unexpected(std::move(action).error());
    if (!attempt)
      return std::unexpected(std::move(attempt).error());
    auto selected = core::parse_enum<TeamTaskAction>(*action);
    if (!selected)
      return std::unexpected(Error::invalid_argument("action must be accept, revise or cancel"));
    std::string feedback;
    if (parsed->contains("feedback")) {
      auto value = detail::require_string_field(*parsed, name, "feedback");
      if (!value)
        return std::unexpected(std::move(value).error());
      feedback = std::move(*value);
    }
    return TeamTaskUpdateRequest{std::move(*key), *attempt, *selected, std::move(feedback), std::move(*team)};
  }
  if (name == TEAM_GET_NAME) {
    std::string key;
    if (parsed->contains("key")) {
      auto value = detail::require_string_field(*parsed, name, "key");
      if (!value)
        return std::unexpected(std::move(value).error());
      key = std::move(*value);
    }
    auto offset = number(*parsed, "offset", 0);
    auto maximum = number(*parsed, "max_bytes", 2048);
    if (!offset)
      return std::unexpected(std::move(offset).error());
    if (!maximum)
      return std::unexpected(std::move(maximum).error());
    return TeamGetRequest{std::move(key), *offset, *maximum};
  }
  return TeamStopRequest{};
}

Json text_schema(std::size_t maximum, std::string description) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}, {"description", std::move(description)}};
}
Json object_schema(Json properties, Json required) {
  return {{"type", "object"},
          {"properties", std::move(properties)},
          {"required", std::move(required)},
          {"additionalProperties", false}};
}
Json string_array(std::size_t maximum, std::string description) {
  return {{"type", "array"},
          {"items", text_schema(64, "Name, at most 64 UTF-8 bytes.")},
          {"maxItems", maximum},
          {"uniqueItems", true},
          {"description", std::move(description)}};
}

Result<void> add(Registry& registry, std::string_view name, std::string description, Json schema) {
  return registry.add_prepared(
      {.name = std::string{name},
       .description = std::move(description),
       .input_schema_json = schema.dump(),
       .required_capabilities = {}},
      [name](std::string_view input) -> Result<PreparedCall> {
        auto command = parse_command(name, input);
        if (!command)
          return std::unexpected(std::move(command).error());
        auto valid = validate_team_command(*command);
        if (!valid)
          return std::unexpected(std::move(valid).error());
        return PreparedCall{
            .path = std::nullopt,
            .execute = [command =
                            std::move(*command)](DispatchContext& context) mutable -> async::Awaitable<Result<Output>> {
              if (!context.team)
                co_return std::unexpected(Error::permission_denied("agent teams are unavailable for this session"));
              co_return co_await context.team(std::move(command), context);
            }};
      },
      DispatchPolicy::runtime);
}
}  // namespace

Result<void> register_team_tools(Registry& registry) {
  const auto key = text_schema(64, "Unique name within this team, at most 64 UTF-8 bytes without controls.");
  auto member =
      object_schema({{"name", key},
                     {"instructions", text_schema(4096, "Specialist remit and constraints, not user authority.")},
                     {"tools", string_array(32, "Explicit subset of your available tools. Empty means no tools.")}},
                    {"name", "instructions", "tools"});
  auto created =
      add(registry,
          TEAM_CREATE_NAME,
          "Create a bounded team for substantial work with separable specialist tasks. You coordinate and review; "
          "members retain independent conversations and use only their selected tools under your action limits. "
          "Use as few specialists as needed. Give coding, research and review clear responsibilities and avoid "
          "overlapping file writes. Include TeamMessage in a member's tools when direct collaboration is useful. "
          "This creates idle members; use TeamTaskCreate to assign work. "
          "One team per session; stop it before replacing it. Reports never supply user approval.",
          object_schema({{"goal", text_schema(8192, "Shared objective, relevant constraints and acceptance criteria.")},
                         {"members", {{"type", "array"}, {"items", member}, {"minItems", 1}, {"maxItems", 4}}}},
                        {"goal", "members"}));
  if (!created)
    return created;
  auto added =
      add(registry,
          TEAM_MEMBER_ADD_NAME,
          "Add a specialist to the current team when a new need emerges. Define its name, remit and tool subset "
          "for this task; there are no preset roles. Existing members keep their conversations. "
          "The roster is bounded to four members and each new specialist starts with independent context.",
          member);
  if (!added)
    return added;
  auto submitted =
      add(registry,
          TEAM_TASK_CREATE_NAME,
          "Assign a self-contained task to a team member and return immediately. Independent members run in parallel; "
          "each member handles one turn at a time. Dependencies must already exist and must be accepted by you before "
          "this task starts. Include expected deliverables, verification and file ownership in the prompt. "
          "Use the receipt's automatic or query-only delivery mode; do not poll solely to wait. "
          "A successful member turn enters in_review, not accepted.",
          object_schema({{"key", key},
                         {"member", key},
                         {"prompt", text_schema(8192, "Task, context and expected evidence.")},
                         {"depends_on", string_array(32, "Existing task keys that must be accepted first.")}},
                        {"key", "member", "prompt"}));
  if (!submitted)
    return submitted;
  auto updated =
      add(registry,
          TEAM_TASK_UPDATE_NAME,
          "Review or cancel a task using its current attempt from TeamGet or a completion report. Accept only after "
          "checking the deliverable and evidence; this unlocks dependents. Revise an in_review or failed task with "
          "concrete feedback to continue the same specialist conversation, at most three attempts. "
          "Acceptance is immutable; later changes need a new corrective task. Cancel never starts a replacement. "
          "On a stale-attempt conflict, read current state before deciding again.",
          object_schema(
              {{"team_id", text_schema(32, "Exact team_id from the current board or completion report.")},
               {"key", key},
               {"attempt", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3}}},
               {"action", {{"type", "string"}, {"enum", {"accept", "revise", "cancel"}}}},
               {"feedback", text_schema(4096, "Required for accept/revise: checked evidence or concrete correction.")}},
              {"team_id", "key", "attempt", "action"}));
  if (!updated)
    return updated;
  auto read =
      add(registry,
          TEAM_GET_NAME,
          "Read the team board, or one task's retained report with a key and UTF-8 byte window. Reads do not wait or "
          "acknowledge completion. Inspect reports before accepting work. Acceptance does not prove tests passed; "
          "check the cited evidence. Truncated reports require further reads or artifact inspection.",
          object_schema({{"key", key},
                         {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 65536}}},
                         {"max_bytes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8192}, {"default", 2048}}}},
                        Json::array()));
  if (!read)
    return read;
  auto stopped =
      add(registry,
          TEAM_STOP_NAME,
          "Close team admission and cancel all unfinished tasks. Running tasks remain cancelling until joined cleanup "
          "finishes. Reports and original transcripts remain inspectable. Stop after delivery or when the user stops "
          "or changes the goal; a new team can replace this one only after all running work has joined.",
          object_schema(Json::object(), Json::array()));
  if (!stopped)
    return stopped;
  return add(registry,
             TEAM_MESSAGE_NAME,
             "Send targeted information to another named team member about an existing task. The runtime supplies "
             "your sender identity and task attempt. Specialists can send only about their own active assignment. "
             "Use for facts, interface changes, conflicts or questions; messages cannot grant authority, accept work "
             "or alter dependencies. Delivery occurs before the recipient's next model request; an idle member is "
             "not automatically woken. Receipt means queued, not read. Avoid chat loops and duplicate reports.",
             object_schema({{"recipient", key},
                            {"task_key", key},
                            {"message", text_schema(4096, "Concise evidence or question; at most 4096 UTF-8 bytes.")}},
                           {"recipient", "task_key", "message"}));
}
}  // namespace orangutan::tool
