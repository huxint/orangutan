#pragma once

#include <span>
#include <string_view>

namespace orangutan::config {

    struct ConfigSecretFieldSpec {
        std::string_view field_kind;
        std::string_view key_name;
    };

    [[nodiscard]]
    std::span<const ConfigSecretFieldSpec> config_secret_field_specs();

    [[nodiscard]]
    const ConfigSecretFieldSpec &profile_api_key_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec &qq_client_secret_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec &qq_bot_client_secret_field();

} // namespace orangutan::config

namespace orangutan {

    using config::config_secret_field_specs;
    using config::ConfigSecretFieldSpec;
    using config::profile_api_key_field;
    using config::qq_bot_client_secret_field;
    using config::qq_client_secret_field;

} // namespace orangutan
