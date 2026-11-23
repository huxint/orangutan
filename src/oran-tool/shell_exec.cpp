#include <oran/tool/builtins.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/error.hpp>
#include <oran/core/tool_def.hpp>
#include <oran/io/subprocess.hpp>
#include <oran/tool/registry.hpp>

#include "_impl/parse_input.hpp"

namespace orangutan::tool {
namespace {

constexpr std::string_view kShellExecSchema =
    R"({"type":"object","properties":{)"
    R"("command":{"type":"string","minLength":1,"maxLength":16384,"description":"Shell command executed via /bin/sh -c inside the workspace."},)"
    R"("working_dir":{"type":"string","minLength":1,"default":".","description":"Workspace-relative working directory."},)"
    R"("timeout_ms":{"type":"integer","minimum":1000,"maximum":120000,"default":30000,"description":"Execution timeout in milliseconds (1000-120000)."},)"
    R"("max_output_bytes":{"type":"integer","minimum":1024,"maximum":1048576,"default":65536,"description":"Per-stream stdout/stderr byte cap (1024-1048576)."}},)"
    R"("required":["command"],"additionalProperties":false})";

constexpr std::array<std::string_view, 4> kAllowedFields{"command", "working_dir", "timeout_ms", "max_output_bytes"};

struct ShellExecRequest {
  std::string command;
  std::string working_dir{"."};
  std::chrono::milliseconds timeout{std::chrono::seconds{30}};
  std::size_t max_output_bytes{64U * 1024U};
};

[[nodiscard]] async::Awaitable<core::Result<Output>>
shell_exec_handler(ShellExecRequest request, DispatchContext& ctx) {
  std::string cwd{};
  if (ctx.resolved_path.has_value()) {
    cwd = ctx.resolved_path->absolute_path;
  } else if (ctx.workspace != nullptr) {
    cwd = std::string{ctx.workspace->root()};
  } else if (request.working_dir != ".") {
    cwd = request.working_dir;
  }

  auto ran = co_await io::run_subprocess(ctx.executor,
                                         io::SubprocessOptions{
                                             .command = request.command,
                                             .working_directory = std::move(cwd),
                                             .timeout = request.timeout,
                                             .max_output_bytes = request.max_output_bytes,
                                         });
  if (!ran) {
    co_return std::unexpected(std::move(ran).error());
  }

  std::string text;
  if (ran->timed_out) {
    text = std::format("Command timed out after {} ms", ran->duration.count());
  } else {
    text = std::format("Exit code: {} ({} ms)", ran->exit_code, ran->duration.count());
  }
  if (!ran->stdout_text.empty()) {
    text.append("\n--- stdout ---\n").append(ran->stdout_text);
    if (ran->stdout_truncated) {
      text.append("\n[stdout truncated]");
    }
  }
  if (!ran->stderr_text.empty()) {
    text.append("\n--- stderr ---\n").append(ran->stderr_text);
    if (ran->stderr_truncated) {
      text.append("\n[stderr truncated]");
    }
  }

  nlohmann::json data{
      {"kind", "shell_exec"},
      {"exit_code", ran->exit_code},
      {"timed_out", ran->timed_out},
      {"duration_ms", ran->duration.count()},
      {"stdout", ran->stdout_text},
      {"stderr", ran->stderr_text},
      {"stdout_truncated", ran->stdout_truncated},
      {"stderr_truncated", ran->stderr_truncated},
  };

  co_return Output{
      .text = std::move(text),
      .data_json = data.dump(),
      .usage = ToolUsage{.wall_time = std::chrono::duration_cast<std::chrono::nanoseconds>(ran->duration),
                         .truncated = ran->stdout_truncated || ran->stderr_truncated},
      .is_error = ran->timed_out || ran->exit_code != 0,
  };
}

[[nodiscard]] core::Result<PreparedCall> prepare_shell_exec(std::string_view input_json) {
  auto parsed = detail::parse_input_object(input_json, kShellExecName, kAllowedFields);
  if (!parsed) {
    return std::unexpected(std::move(parsed).error());
  }

  auto command = detail::require_string_field(*parsed, kShellExecName, "command");
  if (!command) {
    return std::unexpected(std::move(command).error());
  }
  if (command->empty() || command->size() > 16384U || command->contains('\0')) {
    return std::unexpected(core::Error::invalid_argument("ShellExec: `command` must be 1-16384 non-NUL bytes"));
  }

  ShellExecRequest request{.command = std::move(*command)};
  if (const auto it = parsed->find("working_dir"); it != parsed->end()) {
    if (!it->is_string() || it->get_ref<const std::string&>().empty() ||
        it->get_ref<const std::string&>().contains('\0')) {
      return std::unexpected(core::Error::invalid_argument("ShellExec: `working_dir` must be a non-empty path"));
    }
    request.working_dir = it->get<std::string>();
  }
  if (const auto it = parsed->find("timeout_ms"); it != parsed->end()) {
    auto value = detail::parse_positive_unsigned(*it, kShellExecName, "timeout_ms");
    if (!value || *value < 1000 || *value > 120000) {
      return std::unexpected(
          core::Error::invalid_argument("ShellExec: `timeout_ms` must be between 1000 and 120000"));
    }
    request.timeout = std::chrono::milliseconds{*value};
  }
  if (const auto it = parsed->find("max_output_bytes"); it != parsed->end()) {
    auto value = detail::parse_positive_unsigned(*it, kShellExecName, "max_output_bytes");
    if (!value || *value < 1024 || *value > 1048576) {
      return std::unexpected(
          core::Error::invalid_argument("ShellExec: `max_output_bytes` must be between 1024 and 1048576"));
    }
    request.max_output_bytes = static_cast<std::size_t>(*value);
  }

  auto path_req = PathRequest{.path = request.working_dir, .intent = PathIntent::read};
  return PreparedCall{
      .path = std::move(path_req),
      .execute = [req = std::move(request)](DispatchContext& ctx) mutable {
        return shell_exec_handler(std::move(req), ctx);
      },
  };
}

}  // namespace

core::Result<void> register_shell_exec(Registry& registry) {
  core::ToolDef def;
  def.name = std::string{kShellExecName};
  def.description =
      "Run a bounded shell command inside the workspace when file tools are insufficient (for example builds, tests, "
      "or local utilities). Requires explicit spawn_subprocess authorization and respects timeout and output caps.";
  def.input_schema_json = std::string{kShellExecSchema};
  def.required_capabilities = {core::Capability::spawn_subprocess};
  return registry.add_prepared(std::move(def), &prepare_shell_exec);
}

}  // namespace orangutan::tool
