#include "test-helpers.hpp"

#include "tools/internal.hpp"
#include "tools/registry/tool-registry.hpp"
#include "utils/file-io.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

using namespace orangutan;
using namespace orangutan::tools;

namespace {

    ToolRegistry make_read_registry(const std::filesystem::path &workspace) {
        ToolRegistry registry;
        register_read_tool(registry, workspace);
        return registry;
    }

    const Tool &read_tool(const ToolRegistry &registry) {
        const auto *tool = registry.find_tool("read");
        REQUIRE(tool != nullptr);
        return *tool;
    }

} // namespace

TEST_CASE("read returns hashline formatted lines for a text file") {
    const auto workspace = testing::unique_test_root("read-text");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "note.txt";
    fileio::write_file(target, "first\nsecond\nthird\n");

    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("1#"));
    CHECK(result.content.contains(":first"));
    CHECK(result.content.contains(":second"));
    CHECK(result.content.contains(":third"));
}

TEST_CASE("read returns error for missing file") {
    const auto workspace = testing::unique_test_root("read-missing");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "ghost.txt";
    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("File not found"));
}

TEST_CASE("read offset and limit paginate large files") {
    const auto workspace = testing::unique_test_root("read-paginate");
    auto registry = make_read_registry(workspace);

    std::string body;
    for (int i = 1; i <= 50; ++i) {
        body += "line-" + std::to_string(i) + "\n";
    }
    const auto target = workspace / "big.txt";
    fileio::write_file(target, body);

    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}, {"offset", 10}, {"limit", 3}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("line-10"));
    CHECK(result.content.contains("line-11"));
    CHECK(result.content.contains("line-12"));
    CHECK_FALSE(result.content.contains("line-13\n"));
    CHECK(result.content.contains("showing 3 of 50 lines"));
}

TEST_CASE("read offset past end returns a friendly hint") {
    const auto workspace = testing::unique_test_root("read-past-end");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "small.txt";
    fileio::write_file(target, "only line\n");
    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}, {"offset", 999}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("No content at offset 999"));
}

TEST_CASE("read rejects offset < 1 and limit < 1") {
    const auto workspace = testing::unique_test_root("read-bad-params");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "x.txt";
    fileio::write_file(target, "x\n");

    const auto bad_offset = registry.execute(ToolUse("r", "read", {{"path", target.string()}, {"offset", 0}}));
    CHECK(bad_offset.is_error);
    CHECK(bad_offset.content.contains("offset"));

    const auto bad_limit = registry.execute(ToolUse("r", "read", {{"path", target.string()}, {"limit", 0}}));
    CHECK(bad_limit.is_error);
    CHECK(bad_limit.content.contains("limit"));
}

TEST_CASE("read detects binary files and reports type+size") {
    const auto workspace = testing::unique_test_root("read-binary");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "blob.bin";
    std::string payload(64, 'A');
    payload[7] = '\0'; // embedded null trips binary detection
    fileio::write_file(target, payload);

    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("Binary file"));
    CHECK(result.content.contains("64 bytes"));
}

TEST_CASE("read returns image block for a small PNG file") {
    const auto workspace = testing::unique_test_root("read-image");
    auto registry = make_read_registry(workspace);

    // minimal 8-byte PNG magic header is enough — we only exercise the
    // content-type branch, not actual image decoding.
    const std::string png_bytes = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("fake-image-body");
    const auto target = workspace / "pixel.png";
    fileio::write_file(target, png_bytes);

    const auto *tool = registry.find_tool("read");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->execute_rich);
    const auto output = tool->execute_rich({{"path", target.string()}});
    CHECK(output.text.contains("Image:"));
    CHECK(output.text.contains("image/png"));
    REQUIRE(output.images.size() == 1);
    CHECK(output.images.front().media_type == "image/png");
    CHECK_FALSE(output.images.front().data.empty());
}

TEST_CASE("read of oversized image returns text-only truncation notice") {
    const auto workspace = testing::unique_test_root("read-image-huge");
    auto registry = make_read_registry(workspace);

    constexpr std::size_t TOO_BIG = (std::size_t{5} * 1024 * 1024) + 1;
    const auto target = workspace / "big.png";
    fileio::write_file(target, std::string(TOO_BIG, 'x'));

    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(result.is_error);
    CHECK(result.content.contains("Image too large"));
}

TEST_CASE("read multi-path preserves input order and isolates per-file errors") {
    const auto workspace = testing::unique_test_root("read-multi");
    auto registry = make_read_registry(workspace);

    const auto a = workspace / "a.txt";
    const auto b = workspace / "missing.txt";
    const auto c = workspace / "c.txt";
    fileio::write_file(a, "alpha\n");
    fileio::write_file(c, "charlie\n");

    const auto result = registry.execute(
        ToolUse("r", "read", {{"paths", nlohmann::json::array({a.string(), b.string(), c.string()})}}));
    REQUIRE_FALSE(result.is_error);

    const auto pos_a = result.content.find("alpha");
    const auto pos_err = result.content.find("File not found");
    const auto pos_c = result.content.find("charlie");
    REQUIRE(pos_a != std::string::npos);
    REQUIRE(pos_err != std::string::npos);
    REQUIRE(pos_c != std::string::npos);
    CHECK(pos_a < pos_err);
    CHECK(pos_err < pos_c);
    CHECK(result.content.contains("=== " + a.string() + " ==="));
    CHECK(result.content.contains("=== " + b.string() + " ==="));
}

TEST_CASE("read rejects providing both path and paths") {
    const auto workspace = testing::unique_test_root("read-both");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "x.txt";
    fileio::write_file(target, "x\n");
    nlohmann::json input = {{"path", target.string()}, {"paths", nlohmann::json::array({target.string()})}};

    const auto result = registry.execute(ToolUse("r", "read", std::move(input)));
    CHECK(result.is_error);
    CHECK(result.content.contains("either"));
}

TEST_CASE("read rejects missing path and paths") {
    const auto workspace = testing::unique_test_root("read-none");
    auto registry = make_read_registry(workspace);

    const auto result = registry.execute(ToolUse("r", "read", nlohmann::json::object()));
    CHECK(result.is_error);
    CHECK(result.content.contains("Required"));
}

TEST_CASE("read denies paths that escape the workspace sandbox") {
    const auto workspace = testing::unique_test_root("read-escape");
    auto registry = make_read_registry(workspace);

    const auto result = registry.execute(ToolUse("r", "read", {{"path", "/etc/passwd"}}));
    CHECK(result.is_error);
    CHECK(result.content.contains("escapes workspace sandbox"));
}

TEST_CASE("read check_permissions denies non-string entries in paths") {
    const auto workspace = testing::unique_test_root("read-perm-entries");
    auto registry = make_read_registry(workspace);

    const auto &tool = read_tool(registry);
    REQUIRE(tool.check_permissions);
    const ToolPermissionContext ctx;

    const ToolUse call("r", "read", {{"paths", nlohmann::json::array({"ok.txt", 5})}});
    const auto decision = tool.check_permissions(call, ctx);
    CHECK_FALSE(decision.is_passthrough);
    REQUIRE(decision.message.has_value());
    CHECK(decision.message->contains("entries must be strings"));
}

TEST_CASE("read prefixes lines with LINE#HASH tags") {
    const auto workspace = testing::unique_test_root("read-hashline");
    auto registry = make_read_registry(workspace);

    const auto target = workspace / "note.txt";
    fileio::write_file(target, "alpha\nbeta\n");

    const auto result = registry.execute(ToolUse("r", "read", {{"path", target.string()}}));
    REQUIRE_FALSE(result.is_error);
    // hashline format is `LINE#HASH:content` — do not over-couple to hash
    // values, but the `#` prefix and `:` separator are load-bearing.
    CHECK(result.content.contains("1#"));
    CHECK(result.content.contains(":alpha"));
    CHECK(result.content.contains(":beta"));
}
