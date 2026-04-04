#pragma once

#include "permissions/permission-types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace orangutan::permissions {

    struct PermissionConfig {
        PermissionMode default_mode = PermissionMode::default_mode;
        std::vector<std::string> allow;
        std::vector<std::string> deny;
        std::vector<std::string> ask;
    };

    struct CLIPermissionOptions {
        std::optional<PermissionMode> permission_mode;
        bool dangerously_skip_permissions = false;
        std::vector<std::string> allowed_tools;
        std::vector<std::string> disallowed_tools;
    };

    std::vector<PermissionRule> load_rules_from_file(const std::filesystem::path &path, PermissionRuleSource source);

    ToolPermissionContext initialize_permission_context(const PermissionConfig &config, const CLIPermissionOptions &cli_options = {},
                                                        const std::filesystem::path &project_root = {});

    ToolPermissionContext add_rule(const ToolPermissionContext &ctx, PermissionRule rule);

    ToolPermissionContext change_mode(const ToolPermissionContext &ctx, PermissionMode new_mode);

    void persist_rule(const PermissionRule &rule, const std::filesystem::path &settings_file);

} // namespace orangutan::permissions

namespace orangutan {

    using permissions::CLIPermissionOptions;
    using permissions::PermissionConfig;
    using permissions::initialize_permission_context;
    using permissions::load_rules_from_file;

} // namespace orangutan
