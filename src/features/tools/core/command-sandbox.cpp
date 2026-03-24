#include "features/tools/core/command-sandbox.hpp"

#include "features/tools/core/internal.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace orangutan {
namespace {

std::string shell_escape(std::string_view value) {
    std::string escaped = "'";
    for (const auto ch : value) {
        if (ch == '\'') {
            escaped += "'\\''";
            continue;
        }
        escaped.push_back(ch);
    }
    escaped += '\'';
    return escaped;
}

std::optional<std::string> find_bwrap_path() {
    const char *path_env = std::getenv("PATH");
    if (path_env == nullptr) {
        return std::nullopt;
    }

    std::stringstream stream(path_env);
    std::string entry;
    while (std::getline(stream, entry, ':')) {
        if (entry.empty()) {
            continue;
        }
        auto candidate = std::filesystem::path(entry) / "bwrap";
        std::error_code ec;
        const auto status = std::filesystem::status(candidate, ec);
        if (!ec && std::filesystem::exists(status) && (status.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) {
            return candidate.string();
        }
    }

    return std::nullopt;
}

void append_ro_bind_if_exists(std::string &command, std::string_view path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return;
    }
    command += " --ro-bind ";
    command += shell_escape(path);
    command.push_back(' ');
    command += shell_escape(path);
}

std::string sandbox_working_dir(const std::string &workspace_root, const std::string &working_dir) {
    const auto normalized_workspace = normalize_tool_path(std::filesystem::path(workspace_root));
    const auto normalized_working_dir = normalize_tool_path(std::filesystem::path(working_dir.empty() ? workspace_root : working_dir));
    if (!is_path_within_workspace(normalized_working_dir, normalized_workspace)) {
        throw std::runtime_error("working directory escapes workspace sandbox: " + normalized_working_dir.string());
    }

    auto relative = normalized_working_dir.lexically_relative(normalized_workspace);
    std::string sandbox_dir = "/workspace";
    if (!relative.empty() && relative != ".") {
        sandbox_dir += "/";
        sandbox_dir += relative.string();
    }
    return sandbox_dir;
}

SandboxedCommand prepare_isolated_command(const std::string &command, const std::string &workspace_root, const std::string &working_dir) {
    if (workspace_root.empty()) {
        throw std::runtime_error("isolated command execution requires a workspace");
    }

    const auto maybe_bwrap = find_bwrap_path();
    if (!maybe_bwrap.has_value()) {
        throw std::runtime_error("isolated sandbox mode requires bubblewrap ('bwrap') on PATH");
    }

    std::string wrapped = shell_escape(*maybe_bwrap);
    wrapped += " --die-with-parent --new-session --unshare-all";
    wrapped += " --proc /proc --dev /dev --tmpfs /tmp";
    wrapped += " --setenv HOME /tmp --setenv TMPDIR /tmp";

    for (const auto &path : std::vector<std::string>{"/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/opt", "/nix", "/run/current-system/sw"}) {
        append_ro_bind_if_exists(wrapped, path);
    }

    wrapped += " --dir /workspace";
    wrapped += " --bind ";
    wrapped += shell_escape(workspace_root);
    wrapped += " /workspace";
    wrapped += " --chdir ";
    wrapped += shell_escape(sandbox_working_dir(workspace_root, working_dir));
    wrapped += " /bin/sh -c ";
    wrapped += shell_escape(command);

    return {
        .command = wrapped,
        .working_dir = {},
    };
}

} // namespace

SandboxedCommand prepare_sandboxed_command(const std::string &command, const std::string &workspace_root, const std::string &working_dir, ToolSandboxMode sandbox_mode) {
    switch (sandbox_mode) {
        case ToolSandboxMode::disabled:
            return {
                .command = command,
                .working_dir = working_dir,
            };
        case ToolSandboxMode::workspace_write:
            if (workspace_root.empty()) {
                throw std::runtime_error("workspace-write sandbox mode requires a workspace");
            }
            return {
                .command = command,
                .working_dir = working_dir,
            };
        case ToolSandboxMode::isolated:
            return prepare_isolated_command(command, workspace_root, working_dir);
    }

    return {
        .command = command,
        .working_dir = working_dir,
    };
}

} // namespace orangutan
