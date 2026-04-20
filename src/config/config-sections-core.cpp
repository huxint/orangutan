#include "config/config-detail.hpp"
#include "config/json-access.hpp"
#include "providers/provider.hpp"
#include "utils/string.hpp"

#include <array>
#include <optional>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::config::detail {
    namespace {

        using config_string_field = string_member_field<Config>;
        constexpr auto ROOT_AGENT_STRING_FIELDS = std::to_array<config_string_field>({
            {"profile", &Config::profile},
            {"model", &Config::model},
            {"workspace", &Config::workspace},
        });
        constexpr auto TOOLS_STRING_FIELDS = std::to_array<config_string_field>({
            {"edit_mode", &Config::edit_mode},
        });
        constexpr auto QQ_STRING_FIELDS = std::to_array<config_string_field>({
            {"app_id", &Config::qq_app_id},
            {"client_secret", &Config::qq_client_secret},
        });

        using fallback_model_string_field = string_member_field<FallbackModelRef>;
        constexpr auto FALLBACK_MODEL_STRING_FIELDS = std::to_array<fallback_model_string_field>({
            {"profile", &FallbackModelRef::profile},
            {"model", &FallbackModelRef::model},
        });

        using profile_string_field = string_member_field<ProfileConfig>;
        constexpr auto PROFILE_STRING_FIELDS = std::to_array<profile_string_field>({
            {"base_url", &ProfileConfig::base_url},
            {"api_key", &ProfileConfig::api_key},
        });

        using memory_string_field = string_member_field<Config::MemoryConfig>;
        constexpr auto MEMORY_STRING_FIELDS = std::to_array<memory_string_field>({
            {"mirror_file", &Config::MemoryConfig::mirror_file},
            {"journal_dir", &Config::MemoryConfig::journal_dir},
        });

        using agent_string_field = string_member_field<AgentConfig>;
        constexpr auto AGENT_STRING_FIELDS = std::to_array<agent_string_field>({
            {"profile", &AgentConfig::profile},
            {"model", &AgentConfig::model},
            {"workspace", &AgentConfig::workspace},
            {"edit_mode", &AgentConfig::edit_mode},
        });

        using qq_bot_string_field = string_member_field<QqBotConfig>;
        constexpr auto QQ_BOT_STRING_FIELDS = std::to_array<qq_bot_string_field>({
            {"name", &QqBotConfig::name},
            {"app_id", &QqBotConfig::app_id},
            {"client_secret", &QqBotConfig::client_secret},
            {"agent", &QqBotConfig::agent},
        });

        void assign_string_array(const nlohmann::json &array, std::vector<std::string> &target) {
            target.clear();
            if (!array.is_array()) {
                return;
            }

            for (const auto &item : array) {
                if (item.is_string()) {
                    target.push_back(item.get<std::string>());
                }
            }
        }

        void assign_fallback_array(const nlohmann::json &array, std::vector<FallbackModelRef> &target) {
            target.clear();
            if (!array.is_array()) {
                return;
            }

            for (const auto &item : array) {
                if (item.is_string()) {
                    target.emplace_back(item.get<std::string>());
                    continue;
                }
                if (!item.is_object()) {
                    continue;
                }

                FallbackModelRef ref;
                assign_string_members(item, ref, FALLBACK_MODEL_STRING_FIELDS);
                if (!ref.model.empty()) {
                    target.push_back(std::move(ref));
                }
            }
        }

        std::optional<ModelCostConfig> parse_model_cost(const nlohmann::json &value) {
            if (!value.is_object()) {
                return std::nullopt;
            }

            ModelCostConfig cost;
            bool has_value = false;
            if (assign_number_member(value, "input", cost, &ModelCostConfig::input)) {
                has_value = true;
            }
            if (assign_number_member(value, "output", cost, &ModelCostConfig::output)) {
                has_value = true;
            }
            return has_value ? std::optional<ModelCostConfig>(cost) : std::nullopt;
        }

        bool valid_thinking_mode(std::string_view thinking) {
            return thinking == "none" || thinking == "low" || thinking == "medium" || thinking == "high" || thinking == "xhigh";
        }

        ModelConfig parse_model_config(const nlohmann::json &model) {
            ModelConfig cfg;
            static_cast<void>(assign_string_member(model, "provider", cfg, &ModelConfig::provider));
            static_cast<void>(assign_string_member(model, "protocol", cfg, &ModelConfig::protocol));
            static_cast<void>(assign_optional_number_member(model, "max_tokens", cfg, &ModelConfig::max_tokens));
            static_cast<void>(assign_optional_number_member(model, "context_window", cfg, &ModelConfig::context_window));
            if (assign_string_member(model, "thinking", cfg, &ModelConfig::thinking)) {
                if (!valid_thinking_mode(cfg.thinking)) {
                    throw std::runtime_error("invalid model thinking value: " + cfg.thinking);
                }
            }
            if (const auto *value = find_member(model, "cost"); value != nullptr) {
                cfg.cost = parse_model_cost(*value);
            }
            if (cfg.provider.empty()) {
                throw std::runtime_error("model config missing provider");
            }
            if (cfg.protocol.empty()) {
                throw std::runtime_error("model config missing protocol");
            }
            static_cast<void>(providers::parse_provider_kind(cfg.provider));
            static_cast<void>(providers::parse_protocol_kind(cfg.protocol));
            return cfg;
        }

    } // namespace

    AgentConfig make_agent_defaults(const Config &cfg) {
        return {
            .profile = cfg.profile,
            .model = cfg.model,
            .fallback_models = cfg.fallback_models,
            .workspace = cfg.workspace,
            .permissions_config = cfg.permissions_config,
            .team_agents = {},
            .edit_mode = cfg.edit_mode,
            .thinking_budget = cfg.thinking_budget,
        };
    }

    void expand_agent_config(AgentConfig &cfg) {
        cfg.profile = expand_env_vars(cfg.profile);
        cfg.model = expand_env_vars(cfg.model);
        cfg.workspace = expand_path_value(expand_env_vars(cfg.workspace));

        for (auto &fallback_model : cfg.fallback_models) {
            fallback_model.profile = expand_env_vars(fallback_model.profile);
            fallback_model.model = expand_env_vars(fallback_model.model);
        }

        for (auto &agent : cfg.team_agents) {
            agent = expand_env_vars(agent);
        }
    }

    void expand_profile_config(ProfileConfig &cfg) {
        cfg.base_url = expand_env_vars(cfg.base_url);
        cfg.api_key = expand_env_vars(cfg.api_key);
        for (auto &[header_name, header_value] : cfg.headers) {
            static_cast<void>(header_name);
            header_value = expand_env_vars(header_value);
        }
        for (auto &[model_name, model_cfg] : cfg.models) {
            static_cast<void>(model_name);
            model_cfg.provider = expand_env_vars(model_cfg.provider);
            model_cfg.protocol = expand_env_vars(model_cfg.protocol);
            model_cfg.thinking = expand_env_vars(model_cfg.thinking);
        }
    }

    Config parse_agent_section(const nlohmann::json &root, Config cfg) {
        const auto *agent = find_object_member(root, "agent");
        if (agent == nullptr) {
            return cfg;
        }

        assign_string_members(*agent, cfg, ROOT_AGENT_STRING_FIELDS);
        if (const auto *value = find_array_member(*agent, "fallback_models"); value != nullptr) {
            assign_fallback_array(*value, cfg.fallback_models);
        }
        static_cast<void>(assign_number_member(*agent, "temperature", cfg, &Config::temperature));
        static_cast<void>(assign_number_member(*agent, "max_iterations", cfg, &Config::max_iterations));
        static_cast<void>(assign_number_member(*agent, "max_tokens", cfg, &Config::max_tokens));
        static_cast<void>(assign_number_member(*agent, "thinking_budget", cfg, &Config::thinking_budget));

        return cfg;
    }

    Config parse_profiles_section(const nlohmann::json &root, Config cfg) {
        const auto *profiles = find_object_member(root, "profiles");
        if (profiles == nullptr) {
            return cfg;
        }

        for (auto it = profiles->begin(); it != profiles->end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }

            ProfileConfig profile_cfg;
            const auto &profile = it.value();

            assign_string_members(profile, profile_cfg, PROFILE_STRING_FIELDS);
            if (const auto *headers = find_object_member(profile, "headers"); headers != nullptr) {
                for (auto header_it = headers->begin(); header_it != headers->end(); ++header_it) {
                    if (header_it.value().is_string()) {
                        profile_cfg.headers.try_emplace(header_it.key(), header_it.value().get<std::string>());
                    }
                }
            }
            if (const auto *models = find_object_member(profile, "models"); models != nullptr) {
                for (auto model_it = models->begin(); model_it != models->end(); ++model_it) {
                    if (model_it.value().is_object()) {
                        profile_cfg.models.insert_or_assign(model_it.key(), parse_model_config(model_it.value()));
                    }
                }
            }

            cfg.profiles.insert_or_assign(it.key(), std::move(profile_cfg));
        }

        return cfg;
    }

    Config parse_tools_section(const nlohmann::json &root, Config cfg) {
        const auto *tools = find_object_member(root, "tools");
        if (tools == nullptr) {
            return cfg;
        }

        if (const auto *array = find_array_member(*tools, "allowed"); array != nullptr) {
            spdlog::warn("tools.allowed is deprecated, use permissions.allow");
            assign_string_array(*array, cfg.permissions_config.allow);
        }
        if (const auto *array = find_array_member(*tools, "denied"); array != nullptr) {
            spdlog::warn("tools.denied is deprecated, use permissions.deny");
            assign_string_array(*array, cfg.permissions_config.deny);
        }

        assign_string_members(*tools, cfg, TOOLS_STRING_FIELDS);

        return cfg;
    }

    void apply_permissions_config(const nlohmann::json &permissions, PermissionConfig &config) {
        if (const auto *value = find_member(permissions, "default_mode"); value != nullptr && value->is_string()) {
            magic_enum::enum_cast<permission_mode>(utils::normalize_enum_token(value->get<std::string>()))
                .transform([&config](permission_mode mode) {
                    config.default_mode = mode;
                    return mode;
                })
                .or_else([value] {
                    spdlog::warn("unknown permissions.default_mode '{}'", value->get<std::string>());
                    return std::optional<permission_mode>{};
                });
        }
        if (const auto *array = find_array_member(permissions, "allow"); array != nullptr) {
            assign_string_array(*array, config.allow);
        }
        if (const auto *array = find_array_member(permissions, "deny"); array != nullptr) {
            assign_string_array(*array, config.deny);
        }
        if (const auto *array = find_array_member(permissions, "ask"); array != nullptr) {
            assign_string_array(*array, config.ask);
        }
        if (const auto *array = find_array_member(permissions, "allowed_tools"); array != nullptr) {
            spdlog::warn("permissions.allowed_tools is deprecated, use permissions.allow");
            assign_string_array(*array, config.allow);
        }
        if (const auto *array = find_array_member(permissions, "denied_tools"); array != nullptr) {
            spdlog::warn("permissions.denied_tools is deprecated, use permissions.deny");
            assign_string_array(*array, config.deny);
        }
    }

    Config parse_permissions_section(const nlohmann::json &root, Config cfg) {
        const auto *permissions = find_object_member(root, "permissions");
        if (permissions == nullptr) {
            return cfg;
        }

        apply_permissions_config(*permissions, cfg.permissions_config);
        return cfg;
    }

    Config parse_session_section(const nlohmann::json &root, Config cfg) {
        const auto *session = find_object_member(root, "session");
        if (session == nullptr) {
            return cfg;
        }

        static_cast<void>(assign_bool_member(*session, "auto_save", cfg, &Config::auto_save));

        return cfg;
    }

    Config parse_memory_section(const nlohmann::json &root, Config cfg) {
        const auto *memory = find_object_member(root, "memory");
        if (memory == nullptr) {
            return cfg;
        }

        static_cast<void>(assign_bool_member(*memory, "mirror_enabled", cfg.memory, &Config::MemoryConfig::mirror_enabled));
        assign_string_members(*memory, cfg.memory, MEMORY_STRING_FIELDS);

        return cfg;
    }

    Config parse_qq_section(const nlohmann::json &root, Config cfg) {
        const auto *qq = find_object_member(root, "qq");
        if (qq == nullptr) {
            return cfg;
        }

        assign_string_members(*qq, cfg, QQ_STRING_FIELDS);

        return cfg;
    }

    Config parse_agents_section(const nlohmann::json &root, Config cfg) {
        const auto *agents = find_object_member(root, "agents");
        if (agents == nullptr) {
            return cfg;
        }

        const auto agent_defaults = make_agent_defaults(cfg);
        for (auto it = agents->begin(); it != agents->end(); ++it) {
            if (!it.value().is_object()) {
                continue;
            }

            auto agent_cfg = agent_defaults;
            const auto &agent = it.value();

            assign_string_members(agent, agent_cfg, AGENT_STRING_FIELDS);
            if (const auto *value = find_array_member(agent, "fallback_models"); value != nullptr) {
                assign_fallback_array(*value, agent_cfg.fallback_models);
            }
            if (const auto *value = find_object_member(agent, "permissions"); value != nullptr) {
                apply_permissions_config(*value, agent_cfg.permissions_config);
            }
            if (const auto *value = find_array_member(agent, "team_agents"); value != nullptr) {
                assign_string_array(*value, agent_cfg.team_agents);
            }
            static_cast<void>(assign_bool_member(agent, "coordinator_mode", agent_cfg, &AgentConfig::coordinator_mode));
            static_cast<void>(assign_number_member(agent, "max_concurrent_agents", agent_cfg, &AgentConfig::max_concurrent_agents));
            static_cast<void>(assign_number_member(agent, "thinking_budget", agent_cfg, &AgentConfig::thinking_budget));

            cfg.agents.insert_or_assign(it.key(), std::move(agent_cfg));
        }

        return cfg;
    }

    Config parse_qq_bots_section(const nlohmann::json &root, Config cfg) {
        const auto *bots = find_array_member(root, "qq_bots");
        if (bots == nullptr) {
            return cfg;
        }

        for (const auto &item : *bots) {
            if (!item.is_object()) {
                continue;
            }

            QqBotConfig bot_cfg;
            assign_string_members(item, bot_cfg, QQ_BOT_STRING_FIELDS);

            cfg.qq_bots.push_back(std::move(bot_cfg));
        }

        return cfg;
    }

} // namespace orangutan::config::detail
