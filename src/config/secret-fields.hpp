#pragma once

#include <span>
#include <string_view>

namespace orangutan::config {

    enum class ConfigSecretSectionMatchKind {
        exact,
        direct_child_table,
    };

    struct ConfigSecretSectionPattern {
        ConfigSecretSectionMatchKind kind;
        std::string_view section;
    };

    struct ConfigSecretFieldSpec {
        std::string_view field_kind;
        std::string_view key_name;
        ConfigSecretSectionPattern section_pattern;
    };

    [[nodiscard]]
    std::span<const ConfigSecretFieldSpec> config_secret_field_specs();

    [[nodiscard]]
    const ConfigSecretFieldSpec &legacy_agent_api_key_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec &named_agent_api_key_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec &qq_client_secret_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec &qq_bot_client_secret_field();

    [[nodiscard]]
    const ConfigSecretFieldSpec *find_config_secret_field_for_section(std::string_view cleaned_section);

} // namespace orangutan::config

namespace orangutan {

    using config::config_secret_field_specs;
    using config::ConfigSecretFieldSpec;
    using config::ConfigSecretSectionMatchKind;
    using config::ConfigSecretSectionPattern;
    using config::find_config_secret_field_for_section;
    using config::legacy_agent_api_key_field;
    using config::named_agent_api_key_field;
    using config::qq_bot_client_secret_field;
    using config::qq_client_secret_field;

} // namespace orangutan
