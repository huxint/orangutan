#pragma once

#include "process/subprocess.hpp"
#include "utils/escape.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace orangutan::tools::file::search {

    inline std::string append_arg(std::string command, std::string_view flag, std::string_view value) {
        command.push_back(' ');
        command += flag;
        command.push_back(' ');
        command += utils::shell_single_quote_escape(value);
        return command;
    }

    /// Walk PATH and return the first existing entry named `binary`.
    [[nodiscard]]
    inline bool binary_available(std::string_view binary) {
        const auto *path_env = std::getenv("PATH");
        if (path_env == nullptr) {
            return false;
        }
        std::string_view remaining{path_env};
        while (!remaining.empty()) {
            const auto sep = remaining.find(':');
            const auto dir = remaining.substr(0, sep);
            if (!dir.empty()) {
                std::error_code ec;
                if (std::filesystem::exists(std::filesystem::path{dir} / binary, ec)) {
                    return true;
                }
            }
            if (sep == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(sep + 1);
        }
        return false;
    }

    /// Build a helpful error when a required binary is missing, including an install hint.
    [[nodiscard]]
    inline std::string missing_binary_error(std::string_view binary, std::string_view hint) {
        std::string out = "error: `";
        out += binary;
        out += "` binary not found on PATH. ";
        out += hint;
        return out;
    }

    struct SearchResult {
        std::string command;
        process::SubprocessResult result;
    };

    [[nodiscard]]
    inline SearchResult run(const std::string &command, const std::filesystem::path &working_dir) {
        process::SubprocessConfig config;
        config.command = command;
        config.working_dir = working_dir.string();
        config.use_shell = true;
        return SearchResult{.command = command, .result = process::run_subprocess(config)};
    }

    /// Format search output. `no_match_exit_ok` controls whether exit 1 counts as "no matches".
    [[nodiscard]]
    inline std::string summarise(const SearchResult &search, std::string_view empty_hint, bool no_match_exit_ok) {
        const auto &result = search.result;
        if (result.timed_out) {
            return "error: search timed out running `" + search.command + "`";
        }
        const bool ok = result.exit_code == 0 || (no_match_exit_ok && result.exit_code == 1);
        if (!ok) {
            std::string out = "error: `";
            out += search.command;
            out += "` exited with code ";
            out += std::to_string(result.exit_code);
            if (!result.stderr_output.empty()) {
                out.push_back('\n');
                out += result.stderr_output;
            }
            return out;
        }
        if (result.stdout_output.empty()) {
            return std::string{empty_hint};
        }
        return result.stdout_output;
    }

} // namespace orangutan::tools::file::search
