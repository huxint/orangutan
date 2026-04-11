#include "config/secret-fields.hpp"

#include "utils/transparent-lookup.hpp"

#include <array>

#include <nlohmann/json.hpp>

namespace orangutan::config {
    namespace {

        constexpr auto secret(std::string_view field_kind, std::string_view key_name, std::string_view parent_key, std::string_view display_prefix,
                              config_secret_container_kind container_kind) -> ConfigSecretFieldSpec {
            return {
                .field_kind = field_kind,
                .key_name = key_name,
                .parent_key = parent_key,
                .display_prefix = display_prefix,
                .container_kind = container_kind,
            };
        }

        constexpr auto CONFIG_SECRET_FIELDS = std::to_array<ConfigSecretFieldSpec>({
            secret("profiles.api_key", "api_key", "profiles", "profiles", config_secret_container_kind::object_entries),
            secret("qq.client_secret", "client_secret", "qq", "qq", config_secret_container_kind::single_object),
            secret("qq_bots.client_secret", "client_secret", "qq_bots", "qq_bots", config_secret_container_kind::array_entries),
        });

        void maybe_add_secret_value(std::vector<ConfigSecretJsonValue> &matches, nlohmann::json &parent, std::string_view key_name, std::string_view field_kind,
                                    std::string display_field) {
            if (!parent.is_object()) {
                return;
            }

            const auto it = utils::transparent_find(parent, key_name);
            if (it == parent.end() || !it->is_string()) {
                return;
            }

            matches.push_back({
                .field_kind = field_kind,
                .display_field = std::move(display_field),
                .value = &it->get_ref<std::string &>(),
            });
        }

    } // namespace

    std::span<const ConfigSecretFieldSpec> config_secret_field_specs() {
        return CONFIG_SECRET_FIELDS;
    }

    std::vector<ConfigSecretJsonValue> collect_config_secret_json_values(nlohmann::json &root) {
        std::vector<ConfigSecretJsonValue> matches;
        for (const auto &spec : config_secret_field_specs()) {
            auto root_it = utils::transparent_find(root, spec.parent_key);
            if (root_it == root.end()) {
                continue;
            }

            switch (spec.container_kind) {
                case config_secret_container_kind::object_entries:
                    if (!root_it->is_object()) {
                        break;
                    }
                    for (auto profile_it = root_it->begin(); profile_it != root_it->end(); ++profile_it) {
                        maybe_add_secret_value(matches, profile_it.value(), spec.key_name, spec.field_kind,
                                               std::string{spec.display_prefix} + "." + profile_it.key() + "." + std::string{spec.key_name});
                    }
                    break;
                case config_secret_container_kind::single_object:
                    maybe_add_secret_value(matches, *root_it, spec.key_name, spec.field_kind, std::string{spec.display_prefix} + "." + std::string{spec.key_name});
                    break;
                case config_secret_container_kind::array_entries:
                    if (!root_it->is_array()) {
                        break;
                    }
                    for (std::size_t index = 0; index < root_it->size(); ++index) {
                        auto &entry = (*root_it)[index];
                        maybe_add_secret_value(matches, entry, spec.key_name, spec.field_kind,
                                               std::string{spec.display_prefix} + "[" + std::to_string(index) + "]." + std::string{spec.key_name});
                    }
                    break;
            }
        }
        return matches;
    }

} // namespace orangutan::config
