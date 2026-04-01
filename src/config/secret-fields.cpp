#include "config/secret-fields.hpp"

#include <array>

namespace orangutan::config {
    namespace {

        constexpr auto secret(std::string_view field_id, std::string_view key_name) -> ConfigSecretFieldSpec {
            return {
                .field_kind = field_id,
                .key_name = key_name,
            };
        }

        constexpr auto config_secret_fields = std::to_array<ConfigSecretFieldSpec>({
            secret("profiles.api_key", "api_key"),
            secret("qq.client_secret", "client_secret"),
            secret("qq_bots.client_secret", "client_secret"),
        });

    } // namespace

    std::span<const ConfigSecretFieldSpec> config_secret_field_specs() {
        return config_secret_fields;
    }

    const ConfigSecretFieldSpec &profile_api_key_field() {
        return config_secret_fields[0];
    }

    const ConfigSecretFieldSpec &qq_client_secret_field() {
        return config_secret_fields[1];
    }

    const ConfigSecretFieldSpec &qq_bot_client_secret_field() {
        return config_secret_fields[2];
    }

} // namespace orangutan::config
