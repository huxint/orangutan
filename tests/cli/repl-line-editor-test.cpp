#include "cli/line-editor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::cli {
    namespace {

        class ScriptedLineEditor final : public LineEditor {
        public:
            explicit ScriptedLineEditor(std::vector<std::optional<std::string>> responses)
            : responses_(std::move(responses)) {}

            [[nodiscard]]
            std::optional<std::string> prompt(std::string_view prompt_text) override {
                prompts.emplace_back(prompt_text);
                if (index_ >= responses_.size()) {
                    return std::nullopt;
                }
                return responses_[index_++];
            }

            void append_history(std::string_view line) override {
                history.emplace_back(line);
            }

            // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
            std::vector<std::string> prompts;
            std::vector<std::string> history;
            // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

        private:
            std::vector<std::optional<std::string>> responses_;
            std::size_t index_ = 0;
        };

        TEST_CASE("read_repl_input_returns_single_line_and_appends_history") {
            ScriptedLineEditor editor({std::optional<std::string>{"hello"}});

            const auto line = read_repl_input(editor);

            INFO("expected read_repl_input to return a value");
            REQUIRE(line.has_value());
            CHECK(*line == "hello");
            CHECK(editor.prompts.size() == 1UL);
            CHECK(editor.prompts[0] == "you> ");
            CHECK(editor.history.size() == 1UL);
            CHECK(editor.history[0] == "hello");
        };

        TEST_CASE("read_repl_input_leaves_continuation_out_of_history") {
            ScriptedLineEditor editor({std::optional<std::string>{"first\\"}, std::optional<std::string>{"second"}});

            const auto line = read_repl_input(editor);

            INFO("expected read_repl_input to return a continued line");
            REQUIRE(line.has_value());
            CHECK(*line == "first\nsecond");
            CHECK(editor.prompts.size() == 2UL);
            CHECK(editor.prompts[0] == "you> ");
            CHECK(editor.prompts[1] == "... ");
            CHECK(editor.history.empty());
        };

        TEST_CASE("read_repl_input_skips_history_for_empty_line") {
            ScriptedLineEditor editor({std::optional<std::string>{""}});

            const auto line = read_repl_input(editor);

            INFO("expected read_repl_input to return an empty line");
            REQUIRE(line.has_value());
            CHECK(line->empty());
            CHECK(editor.history.empty());
        };

        TEST_CASE("read_repl_input_returns_nullopt_on_eof") {
            ScriptedLineEditor editor({std::nullopt});

            const auto line = read_repl_input(editor);

            CHECK_FALSE(line.has_value());
            CHECK(editor.prompts.size() == 1UL);
            CHECK(editor.prompts[0] == "you> ");
            CHECK(editor.history.empty());
        };

        TEST_CASE("read_repl_multiline_collects_lines_until_blank_without_history") {
            ScriptedLineEditor editor({std::optional<std::string>{"first"}, std::optional<std::string>{"second"}, std::optional<std::string>{""}});
            std::ostringstream output;

            const auto text = read_repl_multiline(editor, output);

            CHECK(text == "first\nsecond");
            CHECK(output.str() == "Multi-line mode. Enter an empty line to finish.\n");
            CHECK(editor.prompts.size() == 3UL);
            CHECK(editor.prompts[0] == "... ");
            CHECK(editor.prompts[1] == "... ");
            CHECK(editor.prompts[2] == "... ");
            CHECK(editor.history.empty());
        };

    } // namespace
} // namespace orangutan::cli
