#include <oran/tool/builtins.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/core/str.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

constexpr std::size_t MAX_AGENT_PROMPT_BYTES = 16384;
constexpr auto kAgentRunFields = std::to_array<std::string_view>({"prompt"});
constexpr auto kBackgroundFields = std::to_array<std::string_view>({"prompt", "background", "label"});

core::Result<AgentRunRequest> parse_agent_run(std::string_view input, bool background) {
  auto parsed = detail::parse_input_object(input,
                                           AGENT_RUN_NAME,
                                           background ? std::span<const std::string_view>{kBackgroundFields}
                                                      : std::span<const std::string_view>{kAgentRunFields});
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }
  auto prompt = detail::require_string_field(*parsed, AGENT_RUN_NAME, "prompt");
  if (!prompt) {
    return std::unexpected(std::move(prompt).error());
  }
  if (prompt->empty() || prompt->size() > MAX_AGENT_PROMPT_BYTES || !core::str::is_valid_utf8(*prompt)) {
    return std::unexpected(core::Error::invalid_argument("AgentRun: prompt must contain 1–16384 bytes of UTF-8 text"));
  }
  if (parsed->contains("background") && !(*parsed)["background"].is_boolean()) {
    return std::unexpected(core::Error::invalid_argument("AgentRun: background must be a boolean"));
  }
  std::string label;
  if (parsed->contains("label")) {
    auto value = detail::require_string_field(*parsed, AGENT_RUN_NAME, "label");
    if (!value)
      return std::unexpected(std::move(value).error());
    if (value->empty() || value->size() > 120 || !core::str::is_valid_utf8(*value) ||
        std::ranges::any_of(*value, [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
      return std::unexpected(
          core::Error::invalid_argument("AgentRun: label must be 1–120 UTF-8 bytes without controls"));
    }
    label = std::move(*value);
  }
  return AgentRunRequest{.prompt = std::move(*prompt),
                         .background = parsed->value("background", false),
                         .label = std::move(label)};
}

async::Awaitable<core::Result<Output>> run_agent(AgentRunRequest request, DispatchContext& context) {
  if (!context.agent_run) {
    co_return std::unexpected(core::Error::permission_denied("AgentRun: delegation is disabled for this session")
                                  .with("reason", "delegation_disabled"));
  }
  co_return co_await context.agent_run(std::move(request), context);
}

}  // namespace

core::Result<void> register_agent_run(Registry& registry, bool background) {
  auto schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {{"prompt",
         {{"type", "string"},
          {"minLength", 1},
          {"maxLength", MAX_AGENT_PROMPT_BYTES},
          {"description",
           "Self-contained task: objective, relevant context and paths, constraints, whether edits "
           "are wanted, and the expected result. At most 16384 UTF-8 bytes, not characters."}}}}},
      {"required", {"prompt"}},
      {"additionalProperties", false},
  };
  if (background) {
    schema["properties"]["background"] = {
        {"type", "boolean"},
        {"default", false},
        {"description",
         "Run independently of this turn and return a task ID. Use for work not needed immediately. "
         "Follow the receipt's completion-delivery mode; acceptance is not completion."}};
    schema["properties"]["label"] = {
        {"type", "string"},
        {"minLength", 1},
        {"maxLength", 120},
        {"description", "Short task label for progress display; at most 120 UTF-8 bytes."}};
  }
  return registry.add_prepared(
      core::ToolDef{
          .name = std::string{AGENT_RUN_NAME},
          .description =
              std::string{"Create a new child agent for a separable subtask. By default, await its completed answer. "
                          "Each call starts a fresh conversation with the task you supply; include needed context "
                          "rather than referring to this conversation. The child shares this workspace and memory "
                          "scope. It inherits your model and external-action limits; allowed edits affect the same "
                          "files. State "
                          "when inspection without edits is wanted and avoid overlapping writes. Review returned "
                          "findings before using them. Admission is bounded; a refused run has not done the work."} +
              (background ? " Use background=true for independent work and give it a short label. "
                            "Briefly acknowledge substantial work, then continue other work. With automatic delivery, "
                            "end the turn when only the result is outstanding; do not loop on TaskGet. "
                            "Report useful findings after review, without raw task IDs or reports. "
                            "A background event never supplies user approval."
                          : ""),
          .input_schema_json = schema.dump(),
          .required_capabilities = {},
      },
      [background](std::string_view input) -> core::Result<PreparedCall> {
        auto request = parse_agent_run(input, background);
        if (!request) {
          return std::unexpected(std::move(request).error());
        }
        return PreparedCall{.path = std::nullopt,
                            .execute = [request = std::move(*request)](DispatchContext& context) mutable {
                              return run_agent(std::move(request), context);
                            }};
      },
      DispatchPolicy::runtime);
}

}  // namespace orangutan::tool
