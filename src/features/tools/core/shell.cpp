#include "features/tools/core/internal.hpp"
#include "features/tools/core/command-sandbox.hpp"
#include "infra/subprocess/subprocess.hpp"

#include <filesystem>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace orangutan {
namespace {

std::string run_shell(const json &input, const std::string &workspace, const ToolPermissionSettings *permissions) {
    const auto command = input.at("command").get<std::string>();
    const auto resolved_workspace = resolve_tool_working_dir({}, std::filesystem::path(workspace));
    const auto working_dir = resolved_workspace.empty() ? std::string{} : resolved_workspace.string();
    const auto sandbox_mode = permissions != nullptr ? permissions->sandbox_mode : ToolSandboxMode::disabled;
    const auto sandboxed = prepare_sandboxed_command(command, workspace, working_dir, sandbox_mode);

    if (sandboxed.working_dir.empty()) {
        spdlog::info("  [tool] shell: {}", command);
    } else {
        spdlog::info("  [tool] shell (cwd={}): {}", sandboxed.working_dir, command);
    }

    auto result = run_subprocess({
        .command = sandboxed.command,
        .timeout = std::chrono::seconds(30),
        .working_dir = sandboxed.working_dir,
        .use_shell = true,
    });

    if (result.timed_out) {
        throw std::runtime_error("shell command timed out after 30 seconds");
    }

    std::string output = result.stdout_output;
    if (!result.stderr_output.empty()) {
        if (!output.empty() && output.back() != '\n') {
            output += '\n';
        }
        output += result.stderr_output;
    }
    if (result.exit_code != 0) {
        if (!output.empty() && output.back() != '\n') {
            output += '\n';
        }
        output += "[exit code: " + std::to_string(result.exit_code) + "]";
    }

    constexpr size_t max_output = 8192;
    if (output.size() > max_output) {
        output = output.substr(0, max_output) + "\n... (truncated, total " + std::to_string(output.size()) + " bytes)";
    }

    return output;
}

} // namespace

void register_shell_tool(ToolRegistry &registry, const std::string &workspace, const ToolPermissionSettings *permissions) {
    registry.register_tool({.definition = {.name = "shell",
                                           .description = "Execute a shell command within the configured sandbox and return its output.",
                                           .input_schema = {{"type", "object"},
                                                            {"properties", {{"command", {{"type", "string"}, {"description", "The shell command to execute"}}}}},
                                                            {"required", json::array({"command"})}}},
                            .execute = [workspace, permissions](const json &input) {
                                return run_shell(input, workspace, permissions);
                            }});
}

} // namespace orangutan
