#include "config/secret-protection.hpp"
#include "config/secret-fields.hpp"
#include "utils/file-io.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <spdlog/common.h>
#include <random>
#include <string>
#include <string_view>

namespace orangutan::config {
    namespace {

        [[nodiscard]]
        bool is_env_reference(std::string_view value) {
            return value.size() >= 4 && value.starts_with("${") && value.ends_with('}');
        }

        [[nodiscard]]
        std::string read_config_file(const std::filesystem::path &path) {
            try {
                return fileio::read_file(path);
            } catch (const std::runtime_error &) {
                throw ConfigSecretProtectionError("Failed to read config file: " + path.string());
            }
        }

        void write_config_file(const std::filesystem::path &path, const std::string &content) {
            try {
                fileio::write_file_binary(path, content);
            } catch (const std::runtime_error &) {
                throw ConfigSecretProtectionError("Failed to write config file: " + path.string());
            }
        }

        [[nodiscard]]
        std::filesystem::perms read_file_permissions(const std::filesystem::path &path) {
            std::error_code ec;
            const auto status = std::filesystem::status(path, ec);
            if (ec) {
                throw ConfigSecretProtectionError("Failed to inspect config file permissions: " + ec.message());
            }
            return status.permissions();
        }

        [[nodiscard]]
        std::string make_temp_filename(const std::filesystem::path &path) {
            thread_local std::mt19937_64 generator{std::random_device{}()};
            return spdlog::fmt_lib::format("{}.{:016x}.tmp", path.filename().string(), generator());
        }

        void prepare_parent_directory(const std::filesystem::path &path) {
            if (path.empty()) {
                return;
            }

            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            if (ec) {
                throw ConfigSecretProtectionError("Failed to prepare config directory for rewrite: " + ec.message());
            }
        }

        void preserve_permissions(const std::filesystem::path &path, std::filesystem::perms permissions) {
            std::error_code ec;
            std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, ec);
            if (ec) {
                std::filesystem::remove(path);
                throw ConfigSecretProtectionError("Failed to preserve config file permissions during rewrite: " + ec.message());
            }
        }

        std::size_t protect_secret_value(ConfigSecretJsonValue &secret, std::string_view password) {
            auto &plaintext = *secret.value;
            if (plaintext.empty() || is_env_reference(plaintext) || is_protected_config_secret(plaintext)) {
                return 0;
            }

            plaintext = protect_config_secret(plaintext, password, secret.field_kind);
            return 1;
        }

    } // namespace

    ProtectConfigSecretsResult protect_config_file_secrets(const std::filesystem::path &path, std::string_view password) {
        if (password.empty()) {
            throw ConfigSecretProtectionError("Protected config secrets require a non-empty password.");
        }

        const auto original = read_config_file(path);
        const auto original_permissions = read_file_permissions(path);
        const bool had_trailing_newline = !original.empty() && original.back() == '\n';

        nlohmann::json root;
        try {
            root = nlohmann::json::parse(original);
        } catch (const nlohmann::json::parse_error &e) {
            throw ConfigSecretProtectionError("Failed to parse JSON config file: " + std::string(e.what()));
        }

        std::size_t protected_count = 0;

        for (auto &secret : collect_config_secret_json_values(root)) {
            protected_count += protect_secret_value(secret, password);
        }

        if (protected_count == 0) {
            return {};
        }

        auto rebuilt = root.dump(2);
        if (had_trailing_newline) {
            rebuilt.push_back('\n');
        }

        auto backup_path = path;
        backup_path += ".bak";
        const auto temp_path = path.parent_path() / make_temp_filename(path);

        prepare_parent_directory(path.parent_path());

        std::error_code ec;
        std::filesystem::copy_file(path, backup_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            throw ConfigSecretProtectionError("Failed to create config backup: " + ec.message());
        }

        try {
            write_config_file(temp_path, rebuilt);
            preserve_permissions(temp_path, original_permissions);
            std::filesystem::rename(temp_path, path, ec);
            if (ec) {
                std::filesystem::remove(temp_path);
                throw ConfigSecretProtectionError("Failed to replace config file atomically: " + ec.message());
            }
        } catch (...) {
            std::filesystem::remove(temp_path);
            throw;
        }

        return {
            .modified = true,
            .protected_count = protected_count,
            .backup_path = std::move(backup_path),
        };
    }

} // namespace orangutan::config
