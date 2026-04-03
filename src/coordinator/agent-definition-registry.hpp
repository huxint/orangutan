#pragma once

#include "coordinator/agent-definition.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace orangutan::coordinator {

    class AgentDefinitionRegistry {
    public:
        AgentDefinitionRegistry();

        void register_definition(AgentDefinition definition);
        void load_builtin_definitions();
        void load_from_directory(const std::string &directory_path);

        [[nodiscard]]
        std::optional<AgentDefinition> find(const std::string &key) const;

        [[nodiscard]]
        std::vector<AgentDefinition> all() const;

        [[nodiscard]]
        bool has(const std::string &key) const;

    private:
        std::unordered_map<std::string, AgentDefinition> definitions_;
    };

} // namespace orangutan::coordinator

namespace orangutan {
    using coordinator::AgentDefinitionRegistry;
} // namespace orangutan
