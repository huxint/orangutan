#include "prompt/prompt-compiler.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace orangutan::prompt;

namespace {

    TEST_CASE("compile_prompt_keeps_must_keep_sections_when_budget_is_tight") {
        PromptBuildInput input;
        input.token_budget = 1;
        input.sections = {
            PromptSection{
                .id = "must_a",
                .kind = prompt_section_kind::static_section,
                .classification = prompt_section_classification::must_keep,
                .priority = 100,
                .content = "AAA",
            },
            PromptSection{
                .id = "must_b",
                .kind = prompt_section_kind::static_section,
                .classification = prompt_section_classification::must_keep,
                .priority = 90,
                .content = "BBB",
            },
            PromptSection{
                .id = "optional_c",
                .kind = prompt_section_kind::dynamic_section,
                .classification = prompt_section_classification::optional,
                .priority = 80,
                .content = "CCC",
            },
        };

        const auto result = compile_prompt(input);

        CHECK(result.full_prompt.contains("AAA"));
        CHECK(result.full_prompt.contains("BBB"));
        CHECK_FALSE(result.full_prompt.contains("CCC"));
        CHECK(result.stats.budget_exceeded);
        CHECK(result.stats.overflow_reason == "must_keep_floor");
        CHECK(result.stats.dropped_section_ids == std::vector<std::string>{"optional_c"});
    }

    TEST_CASE("compile_prompt_orders_sections_by_priority_then_id") {
        PromptBuildInput input;
        input.token_budget = 0;
        input.sections = {
            PromptSection{
                .id = "z_section",
                .kind = prompt_section_kind::static_section,
                .classification = prompt_section_classification::important,
                .priority = 10,
                .content = "Z",
            },
            PromptSection{
                .id = "a_section",
                .kind = prompt_section_kind::static_section,
                .classification = prompt_section_classification::important,
                .priority = 10,
                .content = "A",
            },
            PromptSection{
                .id = "high_section",
                .kind = prompt_section_kind::static_section,
                .classification = prompt_section_classification::important,
                .priority = 50,
                .content = "H",
            },
        };

        const auto result = compile_prompt(input);
        const auto high_pos = result.full_prompt.find("H");
        const auto a_pos = result.full_prompt.find("A");
        const auto z_pos = result.full_prompt.find("Z");

        REQUIRE(high_pos != std::string::npos);
        REQUIRE(a_pos != std::string::npos);
        REQUIRE(z_pos != std::string::npos);
        CHECK(high_pos < a_pos);
        CHECK(a_pos < z_pos);
    }

} // namespace
