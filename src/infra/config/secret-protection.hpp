#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orangutan {

class ConfigSecretProtectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ConfigSecretOptions {
    std::string password_override;
    bool allow_interactive_password = false;
    std::function<std::optional<std::string>(std::string_view prompt)> prompt_callback;
};

struct ProtectConfigSecretsResult {
    bool modified = false;
    size_t protected_count = 0;
    std::filesystem::path backup_path;
};

[[nodiscard]]
std::string default_orangutan_config_path();

[[nodiscard]]
bool is_protected_config_secret(std::string_view value);

[[nodiscard]]
std::string protect_config_secret(std::string_view plaintext, std::string_view password, std::string_view field_kind);

[[nodiscard]]
std::string reveal_config_secret(std::string_view stored_value, std::string_view password, std::string_view field_kind, std::string_view display_field);

[[nodiscard]]
std::string resolve_config_secret_password(const ConfigSecretOptions &options);

[[nodiscard]]
ProtectConfigSecretsResult protect_config_file_secrets(const std::filesystem::path &path, std::string_view password);

} // namespace orangutan
