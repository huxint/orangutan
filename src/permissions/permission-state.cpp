#include "permissions/permission-state.hpp"
#include "permissions/rule-parser.hpp"

#include <fstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace orangutan::permissions {

    namespace {

        std::string rule_to_string(const PermissionRule &rule) {
            if (!rule.content.has_value()) {
                return rule.tool_name;
            }

            const auto &rc = *rule.content;
            switch (rc.match_type) {
                case rule_match_type::prefix:
                    return rule.tool_name + "(" + rc.pattern + ":*)";
                case rule_match_type::wildcard:
                case rule_match_type::exact:
                    return rule.tool_name + "(" + rc.pattern + ")";
            }
            return rule.tool_name;
        }

        std::vector<PermissionRule> parse_string_list(const std::vector<std::string> &items, permission_behavior behavior, permission_rule_source source) {
            std::vector<PermissionRule> rules;
            rules.reserve(items.size());
            for (const auto &item : items) {
                rules.push_back(parse_permission_rule(item, behavior, source));
            }
            return rules;
        }

        void append_rules(std::vector<PermissionRule> &target, std::vector<PermissionRule> source) {
            target.insert(target.end(), std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
        }

        void distribute_rules(const std::vector<PermissionRule> &rules, std::vector<PermissionRule> &allow_rules, std::vector<PermissionRule> &deny_rules,
                              std::vector<PermissionRule> &ask_rules) {
            for (const auto &rule : rules) {
                switch (rule.behavior) {
                    case permission_behavior::allow:
                        allow_rules.push_back(rule);
                        break;
                    case permission_behavior::deny:
                        deny_rules.push_back(rule);
                        break;
                    case permission_behavior::ask:
                        ask_rules.push_back(rule);
                        break;
                }
            }
        }

    } // namespace

    std::vector<PermissionRule> load_rules_from_file(const std::filesystem::path &path, permission_rule_source source) {
        if (path.empty() || !std::filesystem::exists(path)) {
            return {};
        }

        try {
            std::ifstream file(path);
            if (!file.is_open()) {
                spdlog::warn("Could not open permissions file: {}", path.string());
                return {};
            }

            nlohmann::json root;
            file >> root;

            if (!root.is_object()) {
                return {};
            }

            auto perms_it = root.find("permissions");
            if (perms_it == root.end() || !perms_it->is_object()) {
                return {};
            }

            const auto &perms = *perms_it;
            std::vector<PermissionRule> rules;

            if (auto it = perms.find("allow"); it != perms.end() && it->is_array()) {
                for (const auto &item : *it) {
                    if (item.is_string()) {
                        rules.push_back(parse_permission_rule(item.get<std::string>(), permission_behavior::allow, source));
                    }
                }
            }

            if (auto it = perms.find("deny"); it != perms.end() && it->is_array()) {
                for (const auto &item : *it) {
                    if (item.is_string()) {
                        rules.push_back(parse_permission_rule(item.get<std::string>(), permission_behavior::deny, source));
                    }
                }
            }

            if (auto it = perms.find("ask"); it != perms.end() && it->is_array()) {
                for (const auto &item : *it) {
                    if (item.is_string()) {
                        rules.push_back(parse_permission_rule(item.get<std::string>(), permission_behavior::ask, source));
                    }
                }
            }

            return rules;
        } catch (const nlohmann::json::parse_error &e) {
            spdlog::warn("Failed to parse permissions file {}: {}", path.string(), e.what());
            return {};
        } catch (const std::exception &e) {
            spdlog::warn("Error reading permissions file {}: {}", path.string(), e.what());
            return {};
        }
    }

    ToolPermissionContext initialize_permission_context(const PermissionConfig &config, const CLIPermissionOptions &cli_options, const std::filesystem::path &project_root) {
        permission_mode mode = config.default_mode;
        if (cli_options.dangerously_skip_permissions) {
            mode = permission_mode::bypass_permissions;
        } else if (cli_options.mode_override.has_value()) {
            mode = *cli_options.mode_override;
        }

        ToolPermissionContext ctx{
            .mode = mode,
            .allow_rules = {},
            .deny_rules = {},
            .ask_rules = {},
            .is_bypass_available = (mode != permission_mode::bypass_permissions),
        };

        auto user_allow = parse_string_list(config.allow, permission_behavior::allow, permission_rule_source::user_settings);
        auto user_deny = parse_string_list(config.deny, permission_behavior::deny, permission_rule_source::user_settings);
        auto user_ask = parse_string_list(config.ask, permission_behavior::ask, permission_rule_source::user_settings);
        append_rules(ctx.allow_rules, std::move(user_allow));
        append_rules(ctx.deny_rules, std::move(user_deny));
        append_rules(ctx.ask_rules, std::move(user_ask));

        if (!project_root.empty()) {
            auto project_rules = load_rules_from_file(project_root / ".orangutan" / "settings.json", permission_rule_source::project_settings);
            distribute_rules(project_rules, ctx.allow_rules, ctx.deny_rules, ctx.ask_rules);

            auto local_rules = load_rules_from_file(project_root / ".orangutan" / "settings.local.json", permission_rule_source::local_settings);
            distribute_rules(local_rules, ctx.allow_rules, ctx.deny_rules, ctx.ask_rules);
        }

        auto cli_allow = parse_string_list(cli_options.allowed_tools, permission_behavior::allow, permission_rule_source::cli_arg);
        auto cli_deny = parse_string_list(cli_options.disallowed_tools, permission_behavior::deny, permission_rule_source::cli_arg);
        append_rules(ctx.allow_rules, std::move(cli_allow));
        append_rules(ctx.deny_rules, std::move(cli_deny));

        return ctx;
    }

    ToolPermissionContext add_rule(const ToolPermissionContext &ctx, PermissionRule rule) {
        auto copy = ctx;
        switch (rule.behavior) {
            case permission_behavior::allow:
                copy.allow_rules.push_back(std::move(rule));
                break;
            case permission_behavior::deny:
                copy.deny_rules.push_back(std::move(rule));
                break;
            case permission_behavior::ask:
                copy.ask_rules.push_back(std::move(rule));
                break;
        }
        return copy;
    }

    ToolPermissionContext change_mode(const ToolPermissionContext &ctx, permission_mode new_mode) {
        auto copy = ctx;
        copy.mode = new_mode;
        return copy;
    }

    void persist_rule(const PermissionRule &rule, const std::filesystem::path &settings_file) {
        nlohmann::json root = nlohmann::json::object();

        if (std::filesystem::exists(settings_file)) {
            try {
                std::ifstream file(settings_file);
                if (file.is_open()) {
                    file >> root;
                    if (!root.is_object()) {
                        root = nlohmann::json::object();
                    }
                }
            } catch (const std::exception &e) {
                spdlog::warn("Failed to read existing settings file {}: {}", settings_file.string(), e.what());
                root = nlohmann::json::object();
            }
        }

        if (!root.contains("permissions") || !root["permissions"].is_object()) {
            root["permissions"] = nlohmann::json::object();
        }

        std::string array_key;
        switch (rule.behavior) {
            case permission_behavior::allow:
                array_key = "allow";
                break;
            case permission_behavior::deny:
                array_key = "deny";
                break;
            case permission_behavior::ask:
                array_key = "ask";
                break;
        }

        auto &perms = root["permissions"];
        if (!perms.contains(array_key) || !perms[array_key].is_array()) {
            perms[array_key] = nlohmann::json::array();
        }

        perms[array_key].push_back(rule_to_string(rule));

        try {
            if (const auto parent = settings_file.parent_path(); !parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream file(settings_file);
            if (!file.is_open()) {
                throw std::runtime_error("open failed");
            }
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file << root.dump(4) << '\n';
            file.close();
        } catch (const std::exception &e) {
            spdlog::warn("Failed to persist rule to {}: {}", settings_file.string(), e.what());
        }
    }

} // namespace orangutan::permissions
