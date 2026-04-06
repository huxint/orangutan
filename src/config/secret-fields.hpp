#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace orangutan::config {

    enum class config_secret_container_kind : std::uint8_t {
        object_entries,
        single_object,
        array_entries,
    };

    struct ConfigSecretFieldSpec {
        std::string_view field_kind;
        std::string_view key_name;
        std::string_view parent_key;
        std::string_view display_prefix;
        config_secret_container_kind container_kind;
    };

    struct ConfigSecretJsonValue {
        std::string_view field_kind;
        std::string display_field;
        std::string *value;
    };

    [[nodiscard]]
    std::span<const ConfigSecretFieldSpec> config_secret_field_specs();

    [[nodiscard]]
    std::vector<ConfigSecretJsonValue> collect_config_secret_json_values(nlohmann::json &root);

} // namespace orangutan::config

namespace orangutan {

    using config::collect_config_secret_json_values;
    using config::config_secret_field_specs;
    using config::ConfigSecretFieldSpec;
    using config::ConfigSecretJsonValue;

} // namespace orangutan
