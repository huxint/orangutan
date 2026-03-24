#include "app/line-editor.hpp"

#include "support/ut.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orangutan::app {
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

    std::vector<std::string> prompts;
    std::vector<std::string> history;

private:
    std::vector<std::optional<std::string>> responses_;
    size_t index_ = 0;
};

boost::ut::suite repl_line_editor_suite = [] {
    using namespace boost::ut;

    "read_repl_input_returns_single_line_and_appends_history"_test = [] {
        ScriptedLineEditor editor({std::optional<std::string>{"hello"}});

        const auto line = read_repl_input(editor);

        expect(line.has_value() >> fatal) << "expected read_repl_input to return a value";
        expect(*line == "hello");
        expect(editor.prompts.size() == 1_ul);
        expect(editor.prompts[0] == "you> ");
        expect(editor.history.size() == 1_ul);
        expect(editor.history[0] == "hello");
    };

    "read_repl_input_leaves_continuation_out_of_history"_test = [] {
        ScriptedLineEditor editor({std::optional<std::string>{"first\\"}, std::optional<std::string>{"second"}});

        const auto line = read_repl_input(editor);

        expect(line.has_value() >> fatal) << "expected read_repl_input to return a continued line";
        expect(*line == "first\nsecond");
        expect(editor.prompts.size() == 2_ul);
        expect(editor.prompts[0] == "you> ");
        expect(editor.prompts[1] == "... ");
        expect(editor.history.empty());
    };

    "read_repl_input_skips_history_for_empty_line"_test = [] {
        ScriptedLineEditor editor({std::optional<std::string>{""}});

        const auto line = read_repl_input(editor);

        expect(line.has_value() >> fatal) << "expected read_repl_input to return an empty line";
        expect(line->empty());
        expect(editor.history.empty());
    };

    "read_repl_input_returns_nullopt_on_eof"_test = [] {
        ScriptedLineEditor editor({std::nullopt});

        const auto line = read_repl_input(editor);

        expect(not line.has_value());
        expect(editor.prompts.size() == 1_ul);
        expect(editor.prompts[0] == "you> ");
        expect(editor.history.empty());
    };

    "read_repl_multiline_collects_lines_until_blank_without_history"_test = [] {
        ScriptedLineEditor editor({std::optional<std::string>{"first"}, std::optional<std::string>{"second"}, std::optional<std::string>{""}});
        std::ostringstream output;

        const auto text = read_repl_multiline(editor, output);

        expect(text == "first\nsecond");
        expect(output.str() == "Multi-line mode. Enter an empty line to finish.\n");
        expect(editor.prompts.size() == 3_ul);
        expect(editor.prompts[0] == "... ");
        expect(editor.prompts[1] == "... ");
        expect(editor.prompts[2] == "... ");
        expect(editor.history.empty());
    };
};

} // namespace
} // namespace orangutan::app
