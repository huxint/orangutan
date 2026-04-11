#include "tools/shell/command-sandbox.hpp"

#include "tools/internal.hpp"
#include "utils/escape.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::tools {
    namespace {

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
            command += utils::shell_single_quote_escape(path);
            command.push_back(' ');
            command += utils::shell_single_quote_escape(path);
        }

        std::filesystem::path sandbox_working_dir(const std::filesystem::path &workspace_root, const std::filesystem::path &working_dir) {
            const auto normalized_workspace = utils::normalize_path(workspace_root);
            const auto normalized_working_dir = utils::normalize_path(working_dir.empty() ? workspace_root : working_dir);
            if (!utils::path_has_prefix(normalized_working_dir, normalized_workspace)) {
                throw std::runtime_error("working directory escapes workspace sandbox: " + normalized_working_dir.string());
            }

            auto relative = normalized_working_dir.lexically_relative(normalized_workspace);
            auto sandbox_dir = std::filesystem::path{"/workspace"};
            if (!relative.empty() && relative != ".") {
                sandbox_dir /= relative;
            }
            return sandbox_dir;
        }

        SandboxedCommand prepare_isolated_command(std::string_view command, const std::filesystem::path &workspace_root, const std::filesystem::path &working_dir) {
            if (workspace_root.empty()) {
                throw std::runtime_error("isolated command execution requires a workspace");
            }

            const auto maybe_bwrap = find_bwrap_path();
            if (!maybe_bwrap.has_value()) {
                throw std::runtime_error("isolated sandbox mode requires bubblewrap ('bwrap') on PATH");
            }

            std::string wrapped = utils::shell_single_quote_escape(*maybe_bwrap);
            wrapped += " --die-with-parent --new-session --unshare-all";
            wrapped += " --proc /proc --dev /dev --tmpfs /tmp";
            wrapped += " --setenv HOME /tmp --setenv TMPDIR /tmp";

            for (const auto &path : std::vector<std::string>{"/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc", "/opt", "/nix", "/run/current-system/sw"}) {
                append_ro_bind_if_exists(wrapped, path);
            }

            wrapped += " --dir /workspace";
            wrapped += " --bind ";
            wrapped += utils::shell_single_quote_escape(workspace_root.string());
            wrapped += " /workspace";
            wrapped += " --chdir ";
            wrapped += utils::shell_single_quote_escape(sandbox_working_dir(workspace_root, working_dir).string());
            wrapped += " /bin/sh -c ";
            wrapped += utils::shell_single_quote_escape(command);

            return {
                .command = wrapped,
                .working_dir = {},
            };
        }

    } // namespace

    SandboxedCommand prepare_sandboxed_command(std::string_view command, const std::filesystem::path &workspace_root, const std::filesystem::path &working_dir,
                                               tool_sandbox_mode sandbox_mode) {
        switch (sandbox_mode) {
            case tool_sandbox_mode::disabled:
                return {
                    .command = std::string(command),
                    .working_dir = working_dir,
                };
            case tool_sandbox_mode::workspace_write:
                if (workspace_root.empty()) {
                    throw std::runtime_error("workspace-write sandbox mode requires a workspace");
                }
                return {
                    .command = std::string(command),
                    .working_dir = working_dir,
                };
            case tool_sandbox_mode::isolated:
                return prepare_isolated_command(command, workspace_root, working_dir);
        }
        std::unreachable();
    }

} // namespace orangutan::tools
