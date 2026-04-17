#include "test-helpers.hpp"

#include "tools/file/edit/edit-mode.hpp"
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

    ToolRegistry make_edit_registry(const std::filesystem::path &workspace, file::edit_mode mode = file::edit_mode::search_replace) {
        ToolRegistry registry;
        register_edit_tool(registry, workspace, nullptr, mode);
        return registry;
    }

    std::string read_text(const std::filesystem::path &path) {
        return fileio::read_file(path);
    }

    std::string patch_for(const std::filesystem::path &path, std::string_view search, std::string_view replace) {
        std::string out = "*** " + path.string() + "\n<<<<<<< SEARCH\n";
        out += search;
        out += "\n=======\n";
        out += replace;
        out += "\n>>>>>>> REPLACE\n";
        return out;
    }

} // namespace

TEST_CASE("edit applies a single-file search/replace patch") {
    const auto workspace = testing::unique_test_root("edit-basic");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "hello.txt";
    fileio::write_file(target, "alpha\nbeta\ngamma\n");

    const auto patch = patch_for(target, "beta", "BETA");
    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(read_text(target) == "alpha\nBETA\ngamma\n");
    CHECK(result.content.contains("1 hunk"));
    CHECK(result.content.contains("1 file"));
}

TEST_CASE("edit applies multiple hunks on the same file in a single patch") {
    const auto workspace = testing::unique_test_root("edit-multi-hunk");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "poem.txt";
    fileio::write_file(target, "one\ntwo\nthree\nfour\n");

    std::string patch = "*** " + target.string() + "\n";
    patch += "<<<<<<< SEARCH\none\n=======\nONE\n>>>>>>> REPLACE\n";
    patch += "<<<<<<< SEARCH\nfour\n=======\nFOUR\n>>>>>>> REPLACE\n";

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(read_text(target) == "ONE\ntwo\nthree\nFOUR\n");
    CHECK(result.content.contains("2 hunks"));
}

TEST_CASE("edit patch spanning multiple files is atomic across them") {
    const auto workspace = testing::unique_test_root("edit-multi-file");
    auto registry = make_edit_registry(workspace);

    const auto a = workspace / "a.txt";
    const auto b = workspace / "b.txt";
    fileio::write_file(a, "aaa\n");
    fileio::write_file(b, "bbb\n");

    std::string patch;
    patch += patch_for(a, "aaa", "AAA");
    patch += patch_for(b, "bbb", "BBB");

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(read_text(a) == "AAA\n");
    CHECK(read_text(b) == "BBB\n");
    CHECK(result.content.contains("2 files"));
}

TEST_CASE("edit with empty SEARCH creates a new file") {
    const auto workspace = testing::unique_test_root("edit-new-file");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "new.txt";
    std::string patch = "*** " + target.string() + "\n<<<<<<< SEARCH\n=======\nnew body\n>>>>>>> REPLACE\n";

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    REQUIRE_FALSE(result.is_error);
    REQUIRE(std::filesystem::exists(target));
    CHECK(read_text(target) == "new body");
}

TEST_CASE("edit new-file refuses to overwrite an existing path") {
    const auto workspace = testing::unique_test_root("edit-new-clash");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "there.txt";
    fileio::write_file(target, "already here");

    std::string patch = "*** " + target.string() + "\n<<<<<<< SEARCH\n=======\nreplacement\n>>>>>>> REPLACE\n";
    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("file already exists"));
    CHECK(read_text(target) == "already here"); // original preserved
}

TEST_CASE("edit errors when search text is not present") {
    const auto workspace = testing::unique_test_root("edit-miss");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "t.txt";
    fileio::write_file(target, "aaa\n");

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch_for(target, "nope", "anything")}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("search text not found"));
    CHECK(read_text(target) == "aaa\n");
}

TEST_CASE("edit errors when search text is ambiguous") {
    const auto workspace = testing::unique_test_root("edit-ambiguous");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "t.txt";
    fileio::write_file(target, "dup\nmiddle\ndup\n");

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch_for(target, "dup", "UNIQUE")}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("multiple locations"));
    CHECK(read_text(target) == "dup\nmiddle\ndup\n");
}

TEST_CASE("edit on missing file errors and does not create it") {
    const auto workspace = testing::unique_test_root("edit-missing-file");
    auto registry = make_edit_registry(workspace);

    const auto target = workspace / "ghost.txt";
    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch_for(target, "foo", "bar")}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("file not found"));
    CHECK_FALSE(std::filesystem::exists(target));
}

TEST_CASE("edit validates the whole patch before writing any file") {
    const auto workspace = testing::unique_test_root("edit-atomic");
    auto registry = make_edit_registry(workspace);

    const auto a = workspace / "a.txt";
    const auto b = workspace / "b.txt";
    fileio::write_file(a, "AAA\n");
    fileio::write_file(b, "does-not-match\n");

    std::string patch;
    patch += patch_for(a, "AAA", "ZZZ");       // would succeed
    patch += patch_for(b, "missing", "XYZ");   // forces validation failure

    const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
    CHECK(result.is_error);
    // a.txt must not be modified because b.txt failed validation first
    CHECK(read_text(a) == "AAA\n");
    CHECK(read_text(b) == "does-not-match\n");
}

TEST_CASE("edit rejects malformed patches") {
    const auto workspace = testing::unique_test_root("edit-malformed");
    auto registry = make_edit_registry(workspace);
    const auto target = workspace / "f.txt";
    fileio::write_file(target, "body\n");

    SECTION("empty patch") {
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", ""}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("patch is empty"));
    }

    SECTION("hunk before any file header") {
        const std::string patch = "<<<<<<< SEARCH\nbody\n=======\nBODY\n>>>>>>> REPLACE\n";
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("hunk before any"));
    }

    SECTION("missing separator closes search state") {
        std::string patch = "*** " + target.string() + "\n<<<<<<< SEARCH\nbody\n>>>>>>> REPLACE\n";
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("unclosed hunk"));
    }

    SECTION("missing REPLACE marker") {
        std::string patch = "*** " + target.string() + "\n<<<<<<< SEARCH\nbody\n=======\nBODY\n";
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("unclosed hunk"));
    }

    SECTION("file header with no path") {
        const std::string patch = "*** \n<<<<<<< SEARCH\nbody\n=======\nBODY\n>>>>>>> REPLACE\n";
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("file header has no path"));
    }

    SECTION("header-only patch has no hunks") {
        const std::string patch = "*** " + target.string() + "\n";
        const auto result = registry.execute(ToolUse("e", "edit", {{"patch", patch}}));
        CHECK(result.is_error);
        CHECK(result.content.contains("no hunks"));
    }
}

TEST_CASE("edit denies sandbox-escaping paths at permission time") {
    const auto workspace = testing::unique_test_root("edit-escape");
    auto registry = make_edit_registry(workspace);

    const auto *tool = registry.find_tool("edit");
    REQUIRE(tool != nullptr);
    const ToolPermissionContext ctx;

    const std::string patch = "*** /etc/passwd\n<<<<<<< SEARCH\nroot\n=======\npwn\n>>>>>>> REPLACE\n";
    const ToolUse call("e", "edit", {{"patch", patch}});
    const auto decision = tool->check_permissions(call, ctx);
    CHECK_FALSE(decision.is_passthrough);
    REQUIRE(decision.message.has_value());
    CHECK(decision.message->contains("escapes workspace sandbox"));
}

TEST_CASE("edit (hashline mode) applies a replace edit via the tool") {
    const auto workspace = testing::unique_test_root("edit-hashline");
    auto registry = make_edit_registry(workspace, file::edit_mode::hashline);

    const auto target = workspace / "lines.txt";
    fileio::write_file(target, "one\ntwo\nthree\n");

    // Compute the anchor using the tool's read side in hashline mode so the
    // hash is whatever the tool currently produces — keeps the test stable
    // against hash-algorithm changes.
    ToolRegistry read_registry;
    register_read_tool(read_registry, workspace, nullptr, file::edit_mode::hashline);
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

TEST_CASE("edit (hashline mode) rejects unknown op values") {
    const auto workspace = testing::unique_test_root("edit-hashline-bad-op");
    auto registry = make_edit_registry(workspace, file::edit_mode::hashline);

    const auto target = workspace / "f.txt";
    fileio::write_file(target, "x\n");

    nlohmann::json edits = nlohmann::json::array({nlohmann::json{{"op", "shuffle"}}});
    const auto result = registry.execute(ToolUse("e", "edit", {{"path", target.string()}, {"edits", edits}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("unknown edit op"));
}
