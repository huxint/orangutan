#include "tools/script/script-loader.hpp"

#include "tools/shell/command-sandbox.hpp"
#include "tools/internal.hpp"
#include "utils/sender-utils.hpp"
#include "process/subprocess.hpp"

#include <ctre.hpp>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace orangutan::tools {

    // ── Shell Escaping ──────────────────────────────

    std::string shell_escape(const std::string &value) {
        std::string escaped = "'";
        for (char ch : value) {
            if (ch == '\'') {
                escaped += "'\\''";
            } else {
                escaped += ch;
            }
        }
        escaped += '\'';
        return escaped;
    }

    // ── Parameter Substitution ──────────────────────

    std::string substitute_params(const std::string &command_template, const nlohmann::json &input, const std::unordered_map<std::string, std::string> &schema) {
        std::string result;
        result.reserve(command_template.size());
        const auto template_view = std::string_view{command_template};
        const char *pos = template_view.data();
        const char *end = pos + template_view.size();

        for (auto match : ctre::search_all<R"(\$\{(\w+)\})">(template_view)) {
            const auto full = match.get<0>().to_view();
            result.append(pos, full.data() - pos);

            auto param_name = std::string(match.get<1>().to_view());
            if (input.contains(param_name)) {
                const auto &val = input.at(param_name);
                std::string str_val;
                if (val.is_string()) {
                    str_val = val.get<std::string>();
                } else {
                    str_val = val.dump();
                }
                result += shell_escape(str_val);
            } else {
                spdlog::debug("Script tool: parameter '{}' not in input, substituting empty", param_name);
            }

            pos = full.data() + full.size();
        }
        result.append(pos, static_cast<std::size_t>(end - pos));
        return result;
    }

    // ── JSON Schema Generation ──────────────────────

    nlohmann::json generate_input_schema(const std::unordered_map<std::string, std::string> &schema) {
        nlohmann::json properties = nlohmann::json::object();
        nlohmann::json required = nlohmann::json::array();

        for (const auto &[name, type_str] : schema) {
            std::string json_type = type_str;
            if (json_type != "string" && json_type != "integer" && json_type != "number" && json_type != "boolean") {
                spdlog::warn("Script tool: unknown type '{}' for parameter '{}', defaulting to string", type_str, name);
                json_type = "string";
            }
            properties[name] = {{"type", json_type}};
            required.push_back(name);
        }

        return {{"type", "object"}, {"properties", properties}, {"required", required}};
    }

    // ── Script Tool Execution ───────────────────────

    static SubprocessResult execute_script(const std::string &command, const std::string &workspace, const std::string &working_dir, int timeout_seconds,
                                           ToolSandboxMode sandbox_mode) {
        const auto sandboxed = prepare_sandboxed_command(command, workspace, working_dir, sandbox_mode);
        auto pipeline = run_subprocess_sender({
            .command = sandboxed.command,
            .timeout = std::chrono::seconds(timeout_seconds),
            .working_dir = sandboxed.working_dir,
        });
        auto [result] = execution::sync_wait_or_throw(std::move(pipeline), "script tool subprocess pipeline");
        return result;
    }

    // ── Tool Registration Helpers ───────────────────

    static Tool make_script_tool(const ScriptToolConfig &config, const std::string &workspace, const ToolPermissionContext * /*permissions*/,
                                 const ToolRuntimeContext * /*tool_context*/, ApprovalCallback /*approval_callback*/) {
        return Tool{
            .definition = {.name = config.name, .description = config.description, .input_schema = generate_input_schema(config.input_schema)},
            .execute = [config, workspace](const nlohmann::json &input) -> std::string {
                std::string command = substitute_params(config.command, input, config.input_schema);
                const auto resolved_work_dir = resolve_tool_working_dir(config.working_dir, std::filesystem::path(workspace));
                const auto work_dir = resolved_work_dir.empty() ? std::string{} : resolved_work_dir.string();
                const auto sandbox_mode = ToolSandboxMode::disabled;

                spdlog::debug("  [script-tool] {}: {}", config.name, command);

                auto result = execute_script(command, workspace, work_dir, config.timeout, sandbox_mode);

                if (!result.stderr_output.empty()) {
                    spdlog::debug("  [script-tool] {} stderr: {}", config.name, result.stderr_output);
                }

                if (result.timed_out) {
                    throw std::runtime_error("Script tool '" + config.name + "' timed out after " + std::to_string(config.timeout) + " seconds");
                }

                if (result.exit_code != 0) {
                    std::string error_msg = "Command failed with exit code " + std::to_string(result.exit_code);
                    if (!result.stderr_output.empty()) {
                        error_msg += ": " + result.stderr_output;
                    }
                    throw std::runtime_error(error_msg);
                }

                constexpr std::size_t max_output = 8192;
                if (result.stdout_output.size() > max_output) {
                    result.stdout_output = result.stdout_output.substr(0, max_output) + "\n... (truncated, total " + std::to_string(result.stdout_output.size()) + " bytes)";
                }

                return result.stdout_output;
            },
        };
    }

    // ── User Script Tools ───────────────────────────

    void register_script_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace, const ToolPermissionContext *permissions,
                               const ToolRuntimeContext *tool_context, const ApprovalCallback &approval_callback) {
        if (tools.empty()) {
            return;
        }

        int registered = 0;
        for (const auto &config : tools) {
            if (config.name.empty()) {
                spdlog::warn("Script tool missing 'name', skipping");
                continue;
            }
            if (config.command.empty()) {
                spdlog::warn("Script tool '{}' missing 'command', skipping", config.name);
                continue;
            }

            spdlog::info("Registering script tool '{}': {}", config.name, config.command);
            registry.register_tool(make_script_tool(config, workspace, permissions, tool_context, approval_callback));
            ++registered;
        }

        spdlog::info("Registered {} user script tool(s)", registered);
    }

} // namespace orangutan::tools
