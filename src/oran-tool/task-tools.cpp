#include <oran/tool/builtins.hpp>

#include <array>
#include <nlohmann/json.hpp>
#include <oran/core/turn_id.hpp>
#include <string>
#include <utility>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

core::Result<TaskRequest> parse_task(std::string_view input, bool cancel) {
  constexpr auto get_fields = std::to_array<std::string_view>({"task_id", "offset", "max_bytes"});
  constexpr auto cancel_fields = std::to_array<std::string_view>({"task_id"});
  const auto name = cancel ? TASK_CANCEL_NAME : TASK_GET_NAME;
  auto parsed = detail::parse_input_object(input,
                                           name,
                                           cancel ? std::span<const std::string_view>{cancel_fields}
                                                  : std::span<const std::string_view>{get_fields});
  if (!parsed)
    return std::unexpected(std::move(parsed).error());
  auto id = detail::require_string_field(*parsed, name, "task_id");
  if (!id)
    return std::unexpected(std::move(id).error());
  auto key = core::parse_turn_id_hex(*id);
  if (!key || core::is_zero_turn_id(*key))
    return std::unexpected(core::Error::invalid_argument("task_id must be a returned 32-digit task identifier"));
  TaskRequest request{.task_id = core::format_turn_id_hex(*key)};
  for (const auto field : {"offset", "max_bytes"}) {
    if (!parsed->contains(field))
      continue;
    const auto& value = (*parsed)[field];
    const bool offset = std::string_view{field} == "offset";
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > (offset ? 4194304U : 16384U) ||
        (!offset && value.get<std::uint64_t>() == 0))
      return std::unexpected(core::Error::invalid_argument("task result window is out of bounds").with("field", field));
    if (offset)
      request.offset = value.get<std::size_t>();
    else
      request.max_bytes = value.get<std::size_t>();
  }
  return request;
}

async::Awaitable<core::Result<Output>> execute_task(TaskRequest request, bool cancel, DispatchContext& ctx) {
  auto& handler = cancel ? ctx.task_cancel : ctx.task_get;
  if (!handler)
    co_return std::unexpected(core::Error::permission_denied("background task service is unavailable"));
  co_return co_await handler(std::move(request), ctx);
}

core::Result<void> add_task_tool(Registry& registry, bool cancel) {
  auto schema =
      nlohmann::json{{"type", "object"},
                     {"properties",
                      {{"task_id",
                        {{"type", "string"},
                         {"minLength", 32},
                         {"maxLength", 32},
                         {"description", "Task ID returned by an accepted background AgentRun in this session."}}}}},
                     {"required", {"task_id"}},
                     {"additionalProperties", false}};
  if (!cancel) {
    schema["properties"]["offset"] = {
        {"type", "integer"},
        {"minimum", 0},
        {"maximum", 4194304},
        {"default", 0},
        {"description", "UTF-8 byte offset into the retained result; use the returned next_offset."}};
    schema["properties"]["max_bytes"] = {
        {"type", "integer"},
        {"minimum", 1},
        {"maximum", 16384},
        {"default", 8192},
        {"description", "Maximum result bytes in this read. Reads are repeatable and do not consume notifications."}};
  }
  core::ToolDef definition{
      .name = std::string{cancel ? TASK_CANCEL_NAME : TASK_GET_NAME},
      .description =
          cancel
              ? "Request cancellation of one background task owned by this session. Cancelling is not yet cancelled: "
                "cleanup must finish. Results remain inspectable. Do not restart a cancelled task without user intent."
              : "Inspect a background task without waiting for it to finish. Completed results support bounded, "
                "repeatable reads. Use on demand for status or more result text; do not poll in a loop when the "
                "start receipt promises automatic completion. Unknown or expired IDs do not imply a running task.",
      .input_schema_json = schema.dump(),
      .required_capabilities = {}};
  return registry.add_prepared(
      std::move(definition),
      [cancel](std::string_view input) -> core::Result<PreparedCall> {
        auto request = parse_task(input, cancel);
        if (!request)
          return std::unexpected(std::move(request).error());
        return PreparedCall{.path = std::nullopt,
                            .execute = [request = std::move(*request), cancel](DispatchContext& ctx) mutable {
                              return execute_task(std::move(request), cancel, ctx);
                            }};
      },
      DispatchPolicy::runtime);
}
}  // namespace

core::Result<void> register_task_tools(Registry& registry) {
  auto get = add_task_tool(registry, false);
  if (!get)
    return get;
  return add_task_tool(registry, true);
}
}  // namespace orangutan::tool
