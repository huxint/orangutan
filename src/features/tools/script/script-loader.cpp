#include "features/tools/script/script-loader.hpp"

#include "features/tools/core/internal.hpp"
#include "infra/subprocess/subprocess.hpp"

#include <filesystem>
#include <regex>
#include <spdlog/spdlog.h>

namespace orangutan {

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

std::string substitute_params(const std::string &command_template, const json &input, const std::unordered_map<std::string, std::string> &schema) {
    static const std::regex param_re(R"(\$\{(\w+)\})");
    std::string result;
    auto begin = std::sregex_iterator(command_template.begin(), command_template.end(), param_re);
    auto end = std::sregex_iterator();

    size_t last_pos = 0;
    for (auto it = begin; it != end; ++it) {
        const auto &match = *it;
        result.append(command_template, last_pos, static_cast<size_t>(match.position()) - last_pos);

        const std::string param_name = match[1].str();
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

        last_pos = static_cast<size_t>(match.position() + match.length());
    }
    result.append(command_template, last_pos, command_template.size() - last_pos);
    return result;
}

// ── JSON Schema Generation ──────────────────────

json generate_input_schema(const std::unordered_map<std::string, std::string> &schema) {
    json properties = json::object();
    json required = json::array();

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

static SubprocessResult execute_script(const std::string &command, const std::string &working_dir, int timeout_seconds) {
    return run_subprocess({.command = command, .timeout = std::chrono::seconds(timeout_seconds), .working_dir = working_dir});
}

// ── Tool Registration Helpers ───────────────────

static Tool make_script_tool(const ScriptToolConfig &config, const std::string &workspace) {
    return Tool{
        .definition = {.name = config.name, .description = config.description, .input_schema = generate_input_schema(config.input_schema)},
        .execute = [config, workspace](const json &input) -> std::string {
            std::string command = substitute_params(config.command, input, config.input_schema);
            const auto resolved_work_dir = resolve_tool_working_dir(config.working_dir, std::filesystem::path(workspace));
            const auto work_dir = resolved_work_dir.empty() ? std::string{} : resolved_work_dir.string();

            spdlog::debug("  [script-tool] {}: {}", config.name, command);

            auto result = execute_script(command, work_dir, config.timeout);

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

            constexpr size_t max_output = 8192;
            if (result.stdout_output.size() > max_output) {
                result.stdout_output =
                    result.stdout_output.substr(0, max_output) + "\n... (truncated, total " + std::to_string(result.stdout_output.size()) + " bytes)";
            }

            return result.stdout_output;
        },
    };
}


// ── User Script Tools ───────────────────────────

void register_script_tools(ToolRegistry &registry, const std::vector<ScriptToolConfig> &tools, const std::string &workspace) {
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
        registry.register_tool(make_script_tool(config, workspace));
        ++registered;
    }

    spdlog::info("Registered {} user script tool(s)", registered);
}

} // namespace orangutan
