#include <catch2/catch_test_macros.hpp>

#include "coordinator/agent-definition-registry.hpp"
#include "test-helpers.hpp"

#include <type_traits>
#include <filesystem>
#include <fstream>

namespace {

    using Registry = orangutan::coordinator::AgentDefinitionRegistry;
    using FindSignature = std::optional<orangutan::coordinator::AgentDefinition> (Registry::*)(std::string_view) const;

    static_assert(std::is_same_v<decltype(&Registry::load_from_directory), void (Registry::*)(const std::filesystem::path &)>);
    static_assert(std::is_same_v<decltype(&Registry::find), FindSignature>);
    static_assert(std::is_same_v<decltype(&Registry::has), bool (Registry::*)(std::string_view) const>);

} // namespace

TEST_CASE("AgentDefinitionRegistry built-in agents", "[coordinator]") {
    orangutan::coordinator::AgentDefinitionRegistry registry;
    registry.load_builtin_definitions();

    REQUIRE(registry.has("general-purpose"));
    REQUIRE(registry.has("explorer"));
    REQUIRE(registry.has("planner"));

    auto gp = registry.find("general-purpose");
    REQUIRE(gp.has_value());
    REQUIRE(!gp->description.empty());
}

TEST_CASE("agent definition frontmatter csv fields trim whitespace and skip blanks", "[coordinator]") {
    namespace coordinator = orangutan::coordinator;
    namespace testing = orangutan::testing;

    const auto root = testing::unique_test_root("agent-definition-registry");
    const auto agent_path = root / "csv-agent.md";
    std::ofstream(agent_path) << "---\n"
                              << "description: csv trimming baseline\n"
                              << "tools:  read  ,  shell(git:*), , task(list)  ,   \n"
                              << "disallowed_tools:   write , , edit  ,   \n"
                              << "model: test-model\n"
                              << "---\n"
                              << "prompt body\n";

    coordinator::AgentDefinitionRegistry registry;
    registry.load_from_directory(agent_path.parent_path());

    constexpr std::string_view agent_key = "csv-agent";
    CHECK(registry.has(agent_key));

    const auto definition = registry.find(agent_key);
    REQUIRE(definition.has_value());
    CHECK(definition->description == "csv trimming baseline");
    CHECK(definition->tools == std::vector<std::string>{"read", "shell(git:*)", "task(list)"});
    CHECK(definition->disallowed_tools == std::vector<std::string>{"write", "edit"});
    CHECK(definition->model == "test-model");

    std::filesystem::remove_all(root);
}
