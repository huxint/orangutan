#pragma once

#include "coordinator/agent-definition.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace orangutan::coordinator {

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
        std::unordered_map<std::string, AgentDefinition> definitions_;
    };

} // namespace orangutan::coordinator

namespace orangutan {
    using coordinator::AgentDefinitionRegistry;
} // namespace orangutan
