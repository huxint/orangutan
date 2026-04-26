#include "test-helpers.hpp"

#include "tools/internal.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRegistry make_edit_registry(const std::filesystem::path &workspace) {
        ToolRegistry registry;
        register_edit_tool(registry, workspace);
        return registry;
    }

    std::string read_text(const std::filesystem::path &path) {
        return fileio::read_file(path);
    }

} // namespace

TEST_CASE("edit denies sandbox-escaping paths at permission time") {
    const auto workspace = testing::unique_test_root("edit-escape");
    auto registry = make_edit_registry(workspace);

    const auto *tool = registry.find_tool("edit");
    REQUIRE(tool != nullptr);
    const ToolPermissionContext ctx;

    const ToolUse call("e", "edit", {{"path", "/etc/passwd"}, {"edits", nlohmann::json::array({nlohmann::json{{"op", "insert_after"}, {"content", "pwn"}}})}});
    const auto decision = tool->check_permissions(call, ctx);
    CHECK_FALSE(decision.is_passthrough);
    REQUIRE(decision.message.has_value());
    CHECK(decision.message->contains("escapes workspace sandbox"));
}

TEST_CASE("edit applies a replace edit via the tool") {
    const auto workspace = testing::unique_test_root("edit-hashline");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "lines.txt";
    fileio::write_file(target, "one\ntwo\nthree\n");

    // Compute the anchor using the tool's read side so the
    // hash is whatever the tool currently produces — keeps the test stable
    // against hash-algorithm changes.
    ToolRegistry read_registry;
    register_read_tool(read_registry, workspace);
    const auto read_result = read_registry.execute(ToolUse("rd", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(read_result.is_error);

    const auto start = read_result.content.find("2#");
    REQUIRE(start != std::string::npos);
    const auto end = read_result.content.find(':', start);
    REQUIRE(end != std::string::npos);
    const std::string anchor = read_result.content.substr(start, end - start);

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "replace"}, {"anchor", anchor}, {"content", "TWO"}}});
    const auto result = registry.execute(ToolUse("e", "edit", {{"path", target.string()}, {"edits", edits}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(read_text(target) == "one\nTWO\nthree\n");
    CHECK(result.content.contains("Applied 1 edit"));
}

TEST_CASE("edit rejects unknown op values") {
    const auto workspace = testing::unique_test_root("edit-hashline-bad-op");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "f.txt";
    fileio::write_file(target, "x\n");

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "shuffle"}}});
    const auto result = registry.execute(ToolUse("e", "edit", {{"path", target.string()}, {"edits", edits}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("unknown edit op"));
}
