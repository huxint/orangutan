#include "utils/expected-combine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

using namespace orangutan;

TEST_CASE("all_ok with no arguments returns an empty tuple") {
    auto result = utils::all_ok<std::string>();
    REQUIRE(result.has_value());
    static_assert(std::tuple_size_v<std::remove_cvref_t<decltype(*result)>> == 0);
}

TEST_CASE("all_ok collects successful values in argument order") {
    std::expected<int, std::string> a{42};
    std::expected<std::string, std::string> b{std::string{"hello"}};
    auto result = utils::all_ok(a, b);
    REQUIRE(result.has_value());
    auto &[x, y] = *result;
    CHECK(x == 42);
    CHECK(y == "hello");
}

TEST_CASE("all_ok keeps void slots as monostate to preserve positions") {
    std::expected<void, std::string> a{};
    std::expected<int, std::string> b{7};
    std::expected<void, std::string> c{};
    auto result = utils::all_ok(a, b, c);
    REQUIRE(result.has_value());
    auto &[first, middle, last] = *result;
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(first)>, std::monostate>);
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(last)>, std::monostate>);
    CHECK(middle == 7);
}

TEST_CASE("all_ok returns the first error when the first arg fails") {
    std::expected<int, std::string> a{std::unexpected("boom")};
    std::expected<int, std::string> b{2};
    auto result = utils::all_ok(a, b);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == "boom");
}

TEST_CASE("all_ok returns the first error when a middle arg fails") {
    std::expected<int, std::string> a{1};
    std::expected<int, std::string> b{std::unexpected("middle")};
    std::expected<int, std::string> c{std::unexpected("later")};
    auto result = utils::all_ok(a, b, c);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == "middle");
}

TEST_CASE("all_ok returns the last arg's error when only it fails") {
    std::expected<int, std::string> a{1};
    std::expected<void, std::string> b{};
    std::expected<std::string, std::string> c{std::unexpected("tail")};
    auto result = utils::all_ok(a, b, c);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == "tail");
}

TEST_CASE("all_ok moves values out of rvalue expecteds") {
    std::expected<std::unique_ptr<int>, std::string> a{std::make_unique<int>(5)};
    std::expected<std::unique_ptr<int>, std::string> b{std::make_unique<int>(10)};
    auto result = utils::all_ok(std::move(a), std::move(b));
    REQUIRE(result.has_value());
    auto &[pa, pb] = *result;
    REQUIRE(pa != nullptr);
    REQUIRE(pb != nullptr);
    CHECK(*pa == 5);
    CHECK(*pb == 10);
}

TEST_CASE("all_ok composes with std::expected::and_then for dependent steps") {
    auto parse_int = [](std::string_view s) -> std::expected<int, std::string> {
        if (s == "42") {
            return 42;
        }
        return std::unexpected("not 42");
    };
    std::expected<std::string, std::string> raw{std::string{"42"}};
    auto result = utils::all_ok(raw, raw.and_then(parse_int));
    REQUIRE(result.has_value());
    auto &[str, num] = *result;
    CHECK(str == "42");
    CHECK(num == 42);
}
