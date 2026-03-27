#include "app/line-editor.hpp"

#include <replxx.hxx>

#include <memory>

namespace orangutan::app {

    class ReplxxLineEditor::Impl {
    public:
        [[nodiscard]]
        std::optional<std::string> prompt(std::string_view prompt_text) {
            if (const char *line = repl_.input(std::string(prompt_text)); line != nullptr) {
                return std::string(line);
            }
            return std::nullopt;
        }

        void append_history(std::string_view line) {
            repl_.history_add(std::string(line));
        }

    private:
        replxx::Replxx repl_;
    };

    ReplxxLineEditor::ReplxxLineEditor()
    : impl_(std::make_unique<Impl>()) {}

    ReplxxLineEditor::~ReplxxLineEditor() = default;

    std::optional<std::string> ReplxxLineEditor::prompt(std::string_view prompt_text) {
        return impl_->prompt(prompt_text);
    }

    void ReplxxLineEditor::append_history(std::string_view line) {
        impl_->append_history(line);
    }

    std::optional<std::string> read_repl_input(LineEditor &editor) {
        auto line = editor.prompt("you> ");
        if (!line.has_value()) {
            return std::nullopt;
        }
        if (line->empty()) {
            return line;
        }

        const bool has_continuation = line->back() == '\\';
        if (!has_continuation) {
            editor.append_history(*line);
            return line;
        }

        while (!line->empty() && line->back() == '\\') {
            line->pop_back();
            line->push_back('\n');

            auto continuation = editor.prompt("... ");
            if (!continuation.has_value()) {
                break;
            }
            line->append(*continuation);
        }
        return line;
    }

    std::string read_repl_multiline(LineEditor &editor, std::ostream &output) {
        output << "Multi-line mode. Enter an empty line to finish.\n";

        std::string result;
        while (true) {
            auto line = editor.prompt("... ");
            if (!line.has_value() || line->empty()) {
                break;
            }
            if (!result.empty()) {
                result.push_back('\n');
            }
            result.append(*line);
        }
        return result;
    }

} // namespace orangutan::app
