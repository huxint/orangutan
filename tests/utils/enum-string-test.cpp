#include "utils/enum-string.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

using namespace orangutan;

namespace {

    enum class sample_kind : std::uint8_t {
        chat_completions,
        responses,
        messages,
    };

} // namespace

TEST_CASE("enum_name returns magic_enum spelling") {
    CHECK(utils::enum_name(sample_kind::responses) == "responses");
    CHECK(utils::enum_name(sample_kind::chat_completions) == "chat_completions");
}

TEST_CASE("enum_name_kebab returns string_view into static storage") {
    const auto first = utils::enum_name_kebab(sample_kind::chat_completions);
    const auto second = utils::enum_name_kebab(sample_kind::chat_completions);

    CHECK(first == "chat-completions");
    CHECK(utils::enum_name_kebab(sample_kind::messages) == "messages");
    CHECK(first.data() == second.data()); // same static buffer across calls
}

TEST_CASE("enum_name_kebab is usable at compile time") {
    static constexpr auto NAME = utils::enum_name_kebab(sample_kind::chat_completions);
    static_assert(NAME == "chat-completions");
}

TEST_CASE("parse_enum tolerates case and separator variants") {
    CHECK(utils::parse_enum<sample_kind>("CHAT-COMPLETIONS").value() == sample_kind::chat_completions);
    CHECK(utils::parse_enum<sample_kind>("chat_completions").value() == sample_kind::chat_completions);
    CHECK_FALSE(utils::parse_enum<sample_kind>("nope").has_value());
}

TEST_CASE("parse_enum_or falls back on empty or unknown tokens") {
    CHECK(utils::parse_enum_or<sample_kind>("", sample_kind::messages) == sample_kind::messages);
    CHECK(utils::parse_enum_or<sample_kind>("bogus", sample_kind::responses) == sample_kind::responses);
    CHECK(utils::parse_enum_or<sample_kind>("messages", sample_kind::responses) == sample_kind::messages);
}
