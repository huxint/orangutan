#include "features/tools/core/internal.hpp"
#include "features/tools/core/background-completion.hpp"
#include "features/tools/core/command-sandbox.hpp"
#include "infra/execution/sender-utils.hpp"
#include "infra/subprocess/subprocess.hpp"
#include "infra/utf8.hpp"

#include <chrono>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace orangutan {
namespace {

json completion_mode_enum(const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher) {
    if (completion_dispatcher != nullptr && completion_dispatcher->supports_resume_callback()) {
        return json::array({"inbox", "resume"});
    }
    return json::array({"inbox"});
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

json process_summary_json(const BackgroundProcessSummary &summary) {
    json payload = {
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
    payload["exit_code"] = summary.exit_code.has_value() ? json(*summary.exit_code) : json(nullptr);
    payload["signal_number"] = summary.signal_number.has_value() ? json(*summary.signal_number) : json(nullptr);
    return payload;
}

json process_snapshot_json(const BackgroundProcessSnapshot &snapshot) {
    json payload = process_summary_json(snapshot);
    payload["stdout"] = snapshot.stdout_output;
    payload["stderr"] = snapshot.stderr_output;
    payload["stdout_truncated"] = snapshot.stdout_truncated;
    payload["stderr_truncated"] = snapshot.stderr_truncated;
    return payload;
}

BackgroundProcessCompletionPolicy parse_completion_policy(const json &input, const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher) {
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
            prompt = utf8::truncate_valid_prefix(utf8::sanitize(prompt_it->get_ref<const std::string &>()), background_completion_prompt_max_chars, true);
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

                        constexpr size_t max_output = 8192;
                        if (output.size() > max_output) {
                            output = output.substr(0, max_output) + "\n... (truncated, total " + std::to_string(output.size()) + " bytes)";
                        }

                        return output;
                    });

    auto [output] = execution::sync_wait_or_throw(std::move(pipeline), "foreground shell pipeline");
    return output;
}

std::string run_shell(const json &input, const std::string &workspace, const ToolPermissionSettings *permissions,
                      const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
    const auto command = input.at("command").get<std::string>();
    const bool background = input.value("background", false);
    const auto requested_working_dir = input.value("working_dir", std::string{});
    const auto workspace_root = workspace.empty() ? std::filesystem::path{} : std::filesystem::path(workspace);
    const auto resolved_working_dir = resolve_tool_working_dir(requested_working_dir, workspace_root);
    const auto working_dir = resolved_working_dir.empty() ? std::string{} : resolved_working_dir.string();
    const auto sandbox_mode = permissions != nullptr ? permissions->sandbox_mode : ToolSandboxMode::disabled;
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
    json processes = json::array();
    for (const auto &summary : process_manager->list()) {
        processes.push_back(process_summary_json(summary));
    }
    return json{{"processes", std::move(processes)}}.dump(2);
}

std::string poll_process(const json &input, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
    return process_snapshot_json(process_manager->poll(input.at("process_id").get<std::string>())).dump(2);
}

std::string kill_process(const json &input, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
    auto payload = process_snapshot_json(process_manager->kill(input.at("process_id").get<std::string>()));
    payload["message"] = "Termination requested.";
    return payload.dump(2);
}

} // namespace

void register_shell_tool(ToolRegistry &registry, const std::string &workspace, const ToolPermissionSettings *permissions,
                         const std::shared_ptr<BackgroundCompletionDispatcher> &completion_dispatcher, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
    const bool supports_completion_routing = completion_dispatcher != nullptr && completion_dispatcher->supports_completion_routing();
    const bool resume_supported = supports_resume_mode(completion_dispatcher);
    std::string description = "Execute a shell command. Set background=true for long-running commands to return immediately.";
    json properties = {
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
    const json input_schema = {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", json::array({"command"})},
    };

    registry.register_tool({
        .definition =
            {
                .name = "shell",
                .description = description,
                .input_schema = input_schema,
            },
        .execute =
            [workspace, permissions, completion_dispatcher, process_manager](const json &input) {
                return run_shell(input, workspace, permissions, completion_dispatcher, process_manager);
            },
    });
}

void register_process_tools(ToolRegistry &registry, const std::shared_ptr<BackgroundProcessManager> &process_manager) {
    registry.register_tool({.definition = {.name = "process_list",
                                           .description = "List background processes started by this agent runtime.",
                                           .input_schema = {{"type", "object"}, {"properties", json::object()}}},
                            .execute = [process_manager](const json &) {
                                return list_processes(process_manager);
                            }});

    registry.register_tool(
        {.definition = {.name = "process_poll",
                        .description = "Get the latest status and captured output for a background process.",
                        .input_schema = {{"type", "object"},
                                         {"properties", {{"process_id", {{"type", "string"}, {"description", "The process id returned by shell(background=true)"}}}}},
                                         {"required", json::array({"process_id"})}}},
         .execute = [process_manager](const json &input) {
             return poll_process(input, process_manager);
         }});

    registry.register_tool(
        {.definition = {.name = "process_kill",
                        .description = "Stop a background process and return its latest status.",
                        .input_schema = {{"type", "object"},
                                         {"properties", {{"process_id", {{"type", "string"}, {"description", "The process id returned by shell(background=true)"}}}}},
                                         {"required", json::array({"process_id"})}}},
         .execute = [process_manager](const json &input) {
             return kill_process(input, process_manager);
         }});
}

} // namespace orangutan
