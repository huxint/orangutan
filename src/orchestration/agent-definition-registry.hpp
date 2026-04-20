#pragma once

#include "orchestration/agent-definition.hpp"
#include "utils/transparent-lookup.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orangutan::orchestration {

    class AgentDefinitionRegistry {
    public:
        AgentDefinitionRegistry();

        void register_definition(AgentDefinition definition);
        void load_builtin_definitions();
        void load_from_directory(const std::filesystem::path &directory_path);

        [[nodiscard]]
        std::optional<AgentDefinition> find(std::string_view key) const;

        [[nodiscard]]
        std::vector<AgentDefinition> all() const;

        [[nodiscard]]
        bool has(std::string_view key) const;

    private:
        utils::transparent_string_unordered_map<AgentDefinition> definitions_;
    };

} // namespace orangutan::orchestration

namespace orangutan {
    using orchestration::AgentDefinitionRegistry;
} // namespace orangutan
