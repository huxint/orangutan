#include "config/secret-protection.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <spdlog/common.h>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#endif

namespace orangutan::config {
    namespace {

        constexpr const char *PASSWORD_ENV_VAR = "ORANGUTAN_CONFIG_PASSWORD";

#ifdef _WIN32

        [[nodiscard]]
        bool stdin_stdout_are_terminals() {
            return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
        }

        class EchoGuard {
        public:
            EchoGuard() {
                const auto fd = _fileno(stdin);
                if (fd < 0) {
                    return;
                }

                // Windows console echo is controlled via console modes on the input handle.
                handle_ = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
                if (handle_ == INVALID_HANDLE_VALUE || handle_ == nullptr) {
                    return;
                }

                if (GetConsoleMode(handle_, &original_mode_) == 0) {
                    return;
                }

                modified_ = true;
                const auto updated_mode = original_mode_ & ~ENABLE_ECHO_INPUT;
                if (SetConsoleMode(handle_, updated_mode) == 0) {
                    modified_ = false;
                }
            }

            ~EchoGuard() {
                if (!modified_) {
                    return;
                }
                static_cast<void>(SetConsoleMode(handle_, original_mode_));
            }

            EchoGuard(const EchoGuard &) = delete;
            EchoGuard &operator=(const EchoGuard &) = delete;
            EchoGuard(EchoGuard &&) = delete;
            EchoGuard &operator=(EchoGuard &&) = delete;

        private:
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            DWORD original_mode_ = 0;
            bool modified_ = false;
        };

#else

        [[nodiscard]]
        bool is_terminal(FILE *stream) {
            const auto fd = ::fileno(stream);
            if (fd < 0) {
                return false;
            }

            termios state{};
            return tcgetattr(fd, &state) == 0;
        }

        [[nodiscard]]
        bool stdin_stdout_are_terminals() {
            return is_terminal(stdin) && is_terminal(stdout);
        }

        class EchoGuard {
        public:
            EchoGuard()
            : input_fd_(::fileno(stdin)) {
                if (input_fd_ < 0 || tcgetattr(input_fd_, &original_) != 0) {
                    input_fd_ = -1;
                    return;
                }

                modified_ = true;
                auto updated = original_;
                updated.c_lflag &= static_cast<tcflag_t>(~ECHO);
                if (tcsetattr(input_fd_, TCSANOW, &updated) != 0) {
                    modified_ = false;
                }
            }

            ~EchoGuard() {
                if (!modified_) {
                    return;
                }
                static_cast<void>(tcsetattr(input_fd_, TCSANOW, &original_));
            }

            EchoGuard(const EchoGuard &) = delete;
            EchoGuard &operator=(const EchoGuard &) = delete;
            EchoGuard(EchoGuard &&) = delete;
            EchoGuard &operator=(EchoGuard &&) = delete;

        private:
            int input_fd_ = -1;
            termios original_{};
            bool modified_ = false;
        };

#endif

        [[nodiscard]]
        std::string prompt_for_password() {
            spdlog::fmt_lib::print(stderr, "Config password: ");
            std::fflush(stderr);
            EchoGuard echo_guard;
            std::string password;
            if (!std::getline(std::cin, password)) {
                spdlog::fmt_lib::println(stderr, "");
                throw ConfigSecretProtectionError("Unable to read config secret password from the terminal.");
            }
            spdlog::fmt_lib::println(stderr, "");
            if (password.empty()) {
                throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
            }
            return password;
        }

    } // namespace

    std::filesystem::path default_orangutan_config_path() {
        const char *home = std::getenv("HOME");
        return home == nullptr || std::string_view{home}.empty() ? std::filesystem::path{} : std::filesystem::path(home) / ".orangutan" / "config.json";
    }

    std::string resolve_config_secret_password(const ConfigSecretOptions &options) {
        if (!options.password_override.empty()) {
            return options.password_override;
        }

        if (const char *env_password = std::getenv(PASSWORD_ENV_VAR); env_password != nullptr && !std::string_view{env_password}.empty()) {
            return env_password;
        }

        if (options.prompt_callback) {
            if (auto prompted = options.prompt_callback("Config password: "); prompted.has_value() && !prompted->empty()) {
                return *prompted;
            }
        }

        if (options.allow_interactive_password && stdin_stdout_are_terminals()) {
            return prompt_for_password();
        }

        throw ConfigSecretProtectionError("Protected config secrets require a password. Use --config-password, set ORANGUTAN_CONFIG_PASSWORD, or run in an interactive terminal.");
    }

} // namespace orangutan::config
