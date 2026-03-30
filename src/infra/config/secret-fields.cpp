#include "infra/config/secret-fields.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace orangutan {
    namespace {

        using enum ConfigSecretSectionMatchKind;

        enum class section_scan_state {
            unquoted,
            single_quoted,
            double_quoted,
            double_quoted_escape,
        };

        constexpr auto exact_secret(std::string_view field_id, std::string_view key_name, std::string_view section) -> ConfigSecretFieldSpec {
            return {
                .field_kind = field_id,
                .key_name = key_name,
                .section_pattern =
                    {
                        .kind = exact,
                        .section = section,
                    },
            };
        }

        constexpr auto direct_child_secret(std::string_view field_id, std::string_view key_name, std::string_view section_prefix) -> ConfigSecretFieldSpec {
            return {
                .field_kind = field_id,
                .key_name = key_name,
                .section_pattern =
                    {
                        .kind = direct_child_table,
                        .section = section_prefix,
                    },
            };
        }

        constexpr auto config_secret_fields = std::to_array<ConfigSecretFieldSpec>({
            exact_secret("agent.api_key", "api_key", "[agent]"),
            direct_child_secret("agents.api_key", "api_key", "[agents."),
            exact_secret("qq.client_secret", "client_secret", "[qq]"),
            exact_secret("qq_bots.client_secret", "client_secret", "[[qq_bots]]"),
        });

        constexpr bool contains_unquoted_separator(std::string_view input, char separator) noexcept {
            auto state = section_scan_state::unquoted;
            for (const auto ch : input) {
                switch (state) {
                    case section_scan_state::double_quoted_escape:
                        state = section_scan_state::double_quoted;
                        continue;
                    case section_scan_state::double_quoted:
                        if (ch == '\\') {
                            state = section_scan_state::double_quoted_escape;
                            continue;
                        }
                        if (ch == '"') {
                            state = section_scan_state::unquoted;
                        }
                        continue;
                    case section_scan_state::single_quoted:
                        if (ch == '\'') {
                            state = section_scan_state::unquoted;
                        }
                        continue;
                    case section_scan_state::unquoted:
                        if (ch == '"') {
                            state = section_scan_state::double_quoted;
                            continue;
                        }
                        if (ch == '\'') {
                            state = section_scan_state::single_quoted;
                            continue;
                        }
                        if (ch == separator) {
                            return true;
                        }
                        continue;
                }
                std::unreachable();
            }

            return false;
        }

        constexpr bool matches_section(std::string_view cleaned_section, const ConfigSecretFieldSpec &field) noexcept {
            const auto &pattern = field.section_pattern;
            switch (pattern.kind) {
                case ConfigSecretSectionMatchKind::exact:
                    return cleaned_section == pattern.section;
                case ConfigSecretSectionMatchKind::direct_child_table:
                    if (cleaned_section.size() <= pattern.section.size() || !cleaned_section.starts_with(pattern.section) || !cleaned_section.ends_with(']')) {
                        return false;
                    }

                    const auto child_name = cleaned_section.substr(pattern.section.size(), cleaned_section.size() - pattern.section.size() - 1);
                    return !child_name.empty() && !contains_unquoted_separator(child_name, '.');
            }
            std::unreachable();
        }

    } // namespace

    std::span<const ConfigSecretFieldSpec> config_secret_field_specs() {
        return config_secret_fields;
    }

    const ConfigSecretFieldSpec &legacy_agent_api_key_field() {
        return config_secret_fields[0];
    }

    const ConfigSecretFieldSpec &named_agent_api_key_field() {
        return config_secret_fields[1];
    }

    const ConfigSecretFieldSpec &qq_client_secret_field() {
        return config_secret_fields[2];
    }

    const ConfigSecretFieldSpec &qq_bot_client_secret_field() {
        return config_secret_fields[3];
    }

    const ConfigSecretFieldSpec *find_config_secret_field_for_section(std::string_view cleaned_section) {
        const auto *const match = std::ranges::find_if(config_secret_fields, [&](const ConfigSecretFieldSpec &field) {
            return matches_section(cleaned_section, field);
        });

        return match == config_secret_fields.end() ? nullptr : &*match;
    }

} // namespace orangutan
