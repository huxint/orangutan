#include "tools/internal.hpp"
#include "tools/background/background-completion.hpp"
#include "tools/shell/command-sandbox.hpp"
#include "permissions/permission-evaluator.hpp"
#include "utils/sender-utils.hpp"
#include "process/subprocess.hpp"
#include "utils/utf8.hpp"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vector>

namespace orangutan::tools {
    namespace {

        std::string trim_copy(std::string_view value) {
            std::size_t start = 0;
            while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
                ++start;
            }

            std::size_t end = value.size();
            while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
                --end;
            }

            return std::string(value.substr(start, end - start));
        }

        std::vector<std::string> split_compound_command(std::string_view command) {
            std::vector<std::string> parts;
            std::string current;
            bool in_single_quotes = false;
            bool in_double_quotes = false;
            bool escaped = false;

            for (std::size_t index = 0; index < command.size(); ++index) {
                const char ch = command[index];
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }

                if (ch == '\\') {
                    current.push_back(ch);
                    escaped = true;
                    continue;
                }

                if (ch == '\'' && !in_double_quotes) {
                    in_single_quotes = !in_single_quotes;
                    current.push_back(ch);
                    continue;
                }

                if (ch == '"' && !in_single_quotes) {
                    in_double_quotes = !in_double_quotes;
                    current.push_back(ch);
                    continue;
                }

                if (!in_single_quotes && !in_double_quotes) {
                    const bool is_double_separator = (ch == '&' || ch == '|') && index + 1 < command.size() && command[index + 1] == ch;
                    if (ch == ';' || is_double_separator) {
                        auto trimmed = trim_copy(current);
                        if (!trimmed.empty()) {
                            parts.push_back(std::move(trimmed));
                        }
                        current.clear();
                        if (is_double_separator) {
                            ++index;
                        }
                        continue;
                    }
                }

                current.push_back(ch);
            }

            auto trimmed = trim_copy(current);
            if (!trimmed.empty()) {
                parts.push_back(std::move(trimmed));
            }
            return parts;
        }

        bool is_redirection_operator(std::string_view token) {
            return token == ">" || token == ">>" || token == "<" || token == "1>" || token == "1>>" || token == "2>" || token == "2>>";
        }

        std::vector<std::string> tokenize_command(std::string_view command) {
            std::vector<std::string> tokens;
            std::string current;
            bool in_single_quotes = false;
            bool in_double_quotes = false;
            bool escaped = false;

            const auto flush = [&] {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            };

            for (std::size_t index = 0; index < command.size(); ++index) {
                const char ch = command[index];
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }

                if (ch == '\\') {
                    escaped = true;
                    continue;
                }

                if (ch == '\'' && !in_double_quotes) {
                    in_single_quotes = !in_single_quotes;
                    continue;
                }

                if (ch == '"' && !in_single_quotes) {
                    in_double_quotes = !in_double_quotes;
                    continue;
                }

                if (!in_single_quotes && !in_double_quotes) {
                    if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
                        flush();
                        continue;
                    }

                    if (ch == '>' || ch == '<') {
                        flush();
                        std::string op(1, ch);
                        if (index + 1 < command.size() && command[index + 1] == '>') {
                            op.push_back('>');
                            ++index;
                        }
                        tokens.push_back(std::move(op));
                        continue;
                    }

                    if ((ch == '1' || ch == '2') && index + 1 < command.size() && (command[index + 1] == '>' || command[index + 1] == '<')) {
                        flush();
                        std::string op(1, ch);
                        op.push_back(command[index + 1]);
                        ++index;
                        if (index + 1 < command.size() && command[index + 1] == '>') {
                            op.push_back('>');
                            ++index;
                        }
                        tokens.push_back(std::move(op));
                        continue;
                    }
                }

                current.push_back(ch);
            }

            flush();
            return tokens;
        }

        bool should_validate_path_token(std::string_view token, bool force_validation) {
            if (token.empty()) {
                return false;
            }

            if (force_validation) {
                return true;
            }

            return token == "~"
                || token.starts_with("~/")
                || token.starts_with('/')
                || token.starts_with("./")
                || token.starts_with("../");
        }

        PermissionResult validate_shell_path_token(std::string_view token, const std::filesystem::path &workspace_root,
                                                   const std::filesystem::path &working_dir, const ToolPermissionContext &ctx) {
            if (token.empty()) {
                return PermissionResult::passthrough();
            }

            try {
                auto expanded = expand_tool_home_path(std::filesystem::path(token));
                std::filesystem::path candidate;
                if (expanded.is_absolute()) {
                    candidate = normalize_tool_path(expanded);
                } else {
                    const auto base = working_dir.empty() ? normalize_tool_path(workspace_root) : working_dir;
                    candidate = normalize_tool_path(base / expanded);
                }

                if (!is_tool_path_allowed(candidate, workspace_root, &ctx)) {
                    return PermissionResult::deny("Shell path escapes permission scope: " + std::string(token));
                }
            } catch (const std::exception &e) {
                return PermissionResult::deny(e.what());
            }

            return PermissionResult::passthrough();
        }

        PermissionResult check_shell_permissions(const ToolUse &call, const ToolPermissionContext &ctx, const std::filesystem::path &workspace_root) {
            if (!call.input.contains("command") || !call.input["command"].is_string()) {
                return PermissionResult::deny("Shell command is required");
            }

            const auto command = call.input.at("command").get<std::string>();
            std::filesystem::path resolved_working_dir;
            if (call.input.contains("working_dir") && call.input["working_dir"].is_string()) {
                try {
                    resolved_working_dir = resolve_tool_working_dir(call.input.at("working_dir").get<std::string>(), workspace_root, &ctx);
                } catch (const std::exception &e) {
                    return PermissionResult::deny(e.what());
                }
            } else if (!workspace_root.empty()) {
                resolved_working_dir = normalize_tool_path(workspace_root);
            }

            for (const auto &subcommand : split_compound_command(command)) {
                if (auto deny_rule = permissions::find_matching_rule(call.name, subcommand, ctx.deny_rules); deny_rule.has_value()) {
                    return PermissionResult::deny("Shell subcommand blocked by deny rule: " + subcommand);
                }
                if (auto ask_rule = permissions::find_matching_rule(call.name, subcommand, ctx.ask_rules); ask_rule.has_value()) {
                    return PermissionResult::ask("Shell subcommand requires approval: " + subcommand);
                }
            }

            const auto tokens = tokenize_command(command);
            bool next_token_is_path = false;
            for (const auto &token : tokens) {
                if (is_redirection_operator(token)) {
                    next_token_is_path = true;
                    continue;
                }

                if (should_validate_path_token(token, next_token_is_path)) {
                    auto result = validate_shell_path_token(token, workspace_root, resolved_working_dir, ctx);
                    if (!result.is_passthrough) {
                        return result;
                    }
                }
                next_token_is_path = false;
            }

            return PermissionResult::passthrough();
        }

        nlohmann::json completion_mode_enum(const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher) {
            if (completion_dispatcher != nullptr && completion_dispatcher->supports_resume_callback()) {
                return nlohmann::json::array({"inbox", "resume"});
            }
            return nlohmann::json::array({"inbox"});
        }

        bool supports_resume_mode(const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher) {
            return completion_dispatcher != nullptr && completion_dispatcher->supports_resume_callback();
        }

        std::string process_status(const BackgroundProcessSummary &summary) {
            if (summary.running) {
                return summary.kill_requested ? "stopping" : "running";
            }
            if (summary.signal_number.has_value()) {
                return summary.kill_requested ? "killed" : "signaled";
            }
            if (summary.exit_code.has_value() && *summary.exit_code == 0) {
                return "completed";
            }
            return "failed";
        }

        nlohmann::json process_summary_json(const BackgroundProcessSummary &summary) {
            nlohmann::json payload = {
                {"process_id", summary.process_id},
                {"pid", summary.pid},
                {"command", summary.command},
                {"working_dir", summary.working_dir},
                {"status", process_status(summary)},
                {"running", summary.running},
                {"kill_requested", summary.kill_requested},
                {"stdout_bytes", summary.stdout_bytes},
                {"stderr_bytes", summary.stderr_bytes},
            };
            payload["exit_code"] = summary.exit_code.has_value() ? nlohmann::json(*summary.exit_code) : nlohmann::json(nullptr);
            payload["signal_number"] = summary.signal_number.has_value() ? nlohmann::json(*summary.signal_number) : nlohmann::json(nullptr);
            return payload;
        }

        nlohmann::json process_snapshot_json(const BackgroundProcessSnapshot &snapshot) {
            nlohmann::json payload = process_summary_json(snapshot);
            payload["stdout"] = snapshot.stdout_output;
            payload["stderr"] = snapshot.stderr_output;
            payload["stdout_truncated"] = snapshot.stdout_truncated;
            payload["stderr_truncated"] = snapshot.stderr_truncated;
            return payload;
        }

        BackgroundProcessCompletionPolicy parse_completion_policy(const nlohmann::json &input, const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher) {
            std::string mode = "inbox";
            bool prompt_present = false;
            std::string prompt;
            const bool has_on_complete = input.contains("on_complete") && !input.at("on_complete").is_null();
            const bool supports_completion_routing = completion_dispatcher != nullptr && completion_dispatcher->supports_completion_routing();
            const bool resume_supported = supports_resume_mode(completion_dispatcher);

            if (has_on_complete && !supports_completion_routing) {
                throw std::runtime_error("on_complete is not available in this runtime context");
            }

            if (const auto on_complete_it = input.find("on_complete"); on_complete_it != input.end() && !on_complete_it->is_null()) {
                if (!on_complete_it->is_object()) {
                    throw std::runtime_error("on_complete must be an object");
                }

                mode = on_complete_it->value("mode", std::string{"inbox"});
                if (mode != "inbox" && mode != "resume") {
                    throw std::runtime_error("on_complete.mode must be 'inbox' or 'resume'");
                }
                if (mode == "resume" && !resume_supported) {
                    throw std::runtime_error("on_complete.mode 'resume' is not available in this runtime context");
                }

                if (const auto prompt_it = on_complete_it->find("prompt"); prompt_it != on_complete_it->end()) {
                    if (!prompt_it->is_string()) {
                        throw std::runtime_error("on_complete.prompt must be a string");
                    }
                    prompt_present = true;
                    prompt = utf8::sanitize_and_truncate_valid_prefix(prompt_it->get_ref<const std::string &>(), background_completion_prompt_max_chars, true);
                }
            }

            BackgroundProcessCompletionPolicy completion;
            completion.publish_completion_event = supports_completion_routing;
            if (!completion.publish_completion_event) {
                return completion;
            }

            completion.metadata.emplace(std::string(background_completion_mode_metadata_key), mode);
            if (prompt_present) {
                completion.metadata.emplace(std::string(background_completion_prompt_metadata_key), prompt);
            }
            return completion;
        }

        std::string run_foreground_shell(const std::string &display_command, const std::string &sandboxed_command, const std::string &effective_working_dir) {
            if (effective_working_dir.empty()) {
                spdlog::info("  [tool] shell: {}", display_command);
            } else {
                spdlog::info("  [tool] shell (cwd={}): {}", effective_working_dir, display_command);
            }

            auto pipeline = run_subprocess_sender({
                                .command = sandboxed_command,
                                .timeout = std::chrono::seconds(30),
                                .working_dir = effective_working_dir,
                                .use_shell = true,
                            }) |
                            stdexec::then([](SubprocessResult result) {
                                if (result.timed_out) {
                                    throw std::runtime_error("shell command timed out after 30 seconds");
                                }

                                std::string output = std::move(result.stdout_output);
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

                                constexpr std::size_t max_output = 8192;
                                if (output.size() > max_output) {
                                    output = output.substr(0, max_output) + "\n... (truncated, total " + std::to_string(output.size()) + " bytes)";
                                }

                                return output;
                            });

            auto [output] = execution::sync_wait_or_throw(std::move(pipeline), "foreground shell pipeline");
            return output;
        }

        std::string run_shell(const nlohmann::json &input, const std::string &workspace, const ToolPermissionContext *permissions,
                              const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
            const auto command = input.at("command").get<std::string>();
            const bool background = input.value("background", false);
            const auto requested_working_dir = input.value("working_dir", std::string{});
            const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
            const auto resolved_working_dir = resolve_tool_working_dir(requested_working_dir, workspace_root, permissions);
            const auto working_dir = resolved_working_dir.empty() ? std::string{} : resolved_working_dir.string();
            const auto sandbox_mode = tool_sandbox_mode::disabled;
            const auto sandboxed = prepare_sandboxed_command(command, workspace, working_dir, sandbox_mode);
            const auto effective_working_dir = sandboxed.working_dir.empty() ? working_dir : sandboxed.working_dir;

            if (!background) {
                return run_foreground_shell(command, sandboxed.command, effective_working_dir);
            }

            if (effective_working_dir.empty()) {
                spdlog::info("  [tool] shell background: {}", command);
            } else {
                spdlog::info("  [tool] shell background (cwd={}): {}", effective_working_dir, command);
            }

            const auto summary = process_manager->start(
                {
                    .command = sandboxed.command,
                    .working_dir = effective_working_dir,
                    .use_shell = true,
                },
                command, parse_completion_policy(input, completion_dispatcher));
            auto payload = process_summary_json(summary);
            payload["message"] = "Process started in background. Use process_poll, process_list, or process_kill to manage it.";
            return payload.dump(2);
        }

        std::string list_processes(const std::shared_ptr<BackgroundProcessManager> &process_manager) {
            nlohmann::json processes = nlohmann::json::array();
            for (const auto &summary : process_manager->list()) {
                processes.push_back(process_summary_json(summary));
            }
            return nlohmann::json{{"processes", std::move(processes)}}.dump(2);
        }

        std::string poll_process(const nlohmann::json &input, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
            return process_snapshot_json(process_manager->poll(input.at("process_id").get<std::string>())).dump(2);
        }

        std::string kill_process(const nlohmann::json &input, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
            auto payload = process_snapshot_json(process_manager->kill(input.at("process_id").get<std::string>()));
            payload["message"] = "Termination requested.";
            return payload.dump(2);
        }

    } // namespace

    void register_shell_tool(ToolRegistry &registry, const std::string &workspace, const ToolPermissionContext *permissions,
                             const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
        const bool supports_completion_routing = completion_dispatcher != nullptr && completion_dispatcher->supports_completion_routing();
        const bool resume_supported = supports_resume_mode(completion_dispatcher);
        std::string description =
            "Execute a shell command and return its output.\n\n"
            "IMPORTANT: Avoid using this tool to run cat, head, tail, sed, awk, find, or grep commands "
            "when a dedicated tool can accomplish the task. Instead use:\n"
            " - File reading: Use the `read` tool (NOT cat/head/tail)\n"
            " - File editing: Use the `edit` tool (NOT sed/awk)\n"
            " - File creation: Use the `write` tool (NOT echo/cat with heredoc)\n"
            "Reserve this tool for system commands and terminal operations that require shell execution.\n\n"
            "Instructions:\n"
            " - Use absolute paths and avoid `cd` to maintain working directory consistency.\n"
            " - Always quote file paths containing spaces with double quotes.\n"
            " - When issuing multiple independent commands, make separate parallel tool calls.\n"
            " - Chain dependent commands with `&&`; use `;` only when you don't care if earlier commands fail.\n"
            " - For git: prefer new commits over amending; never skip hooks (--no-verify); avoid destructive ops (reset --hard, push --force) unless truly necessary.\n"
            " - Set background=true for long-running commands to return immediately.";
        nlohmann::json properties = {
            {"command", {{"type", "string"}, {"description", "The shell command to execute"}}},
            {"background", {{"type", "boolean"}, {"description", "Run the command in the background and return a process id"}}},
            {"working_dir", {{"type", "string"}, {"description", "Optional working directory inside the workspace"}}},
        };
        if (supports_completion_routing) {
            description += resume_supported ? " Background commands can use on_complete to write an inbox completion record and optionally request runtime-local resume."
                                            : " Background commands can use on_complete to write an inbox completion record.";
            properties["on_complete"] = {
                {"type", "object"},
                {"description", resume_supported ? "Optional completion routing for background commands. Defaults to inbox-only delivery. "
                                                   "Set mode=resume to also request a runtime-local agent resume."
                                                 : "Optional completion routing for background commands. Delivery is inbox-only in this runtime context."},
                {"properties",
                 {
                     {"mode",
                      {
                          {"type", "string"},
                          {"enum", completion_mode_enum(completion_dispatcher)},
                          {"description", "Completion delivery mode for a background command"},
                      }},
                     {"prompt",
                      {
                          {"type", "string"},
                          {"description", "Optional prompt to include in the structured completion payload"},
                      }},
                 }},
            };
        }
        const nlohmann::json input_schema = {
            {"type", "object"},
            {"properties", std::move(properties)},
            {"required", nlohmann::json::array({"command"})},
        };

        registry.register_tool({
            .definition =
                {
                    .name = "shell",
                    .description = description,
                    .input_schema = input_schema,
                },
            .check_permissions =
                [workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace)](const ToolUse &call, const ToolPermissionContext &ctx) {
                    return check_shell_permissions(call, ctx, workspace_root);
                },
            .execute =
                [workspace, permissions, completion_dispatcher, process_manager](const nlohmann::json &input) {
                    return run_shell(input, workspace, permissions, completion_dispatcher, process_manager);
                },
        });
    }

    void register_process_tools(ToolRegistry &registry, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
        registry.register_tool({.definition = {.name = "process_list",
                                               .description = "List background processes started by this agent runtime.",
                                               .input_schema = {{"type", "object"}, {"properties", nlohmann::json::object()}}},
                                .execute = [process_manager](const nlohmann::json &) {
                                    return list_processes(process_manager);
                                }});

        registry.register_tool(
            {.definition = {.name = "process_poll",
                            .description = "Get the latest status and captured output for a background process.",
                            .input_schema = {{"type", "object"},
                                             {"properties", {{"process_id", {{"type", "string"}, {"description", "The process id returned by shell(background=true)"}}}}},
                                             {"required", nlohmann::json::array({"process_id"})}}},
             .execute = [process_manager](const nlohmann::json &input) {
                 return poll_process(input, process_manager);
             }});

        registry.register_tool(
            {.definition = {.name = "process_kill",
                            .description = "Stop a background process and return its latest status.",
                            .input_schema = {{"type", "object"},
                                             {"properties", {{"process_id", {{"type", "string"}, {"description", "The process id returned by shell(background=true)"}}}}},
                                             {"required", nlohmann::json::array({"process_id"})}}},
             .execute = [process_manager](const nlohmann::json &input) {
                 return kill_process(input, process_manager);
             }});
    }

} // namespace orangutan::tools
