#pragma once

#include <span>
#include <string_view>

namespace orangutan {

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

} // namespace orangutan
