#pragma once

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace orangutan::app {

class LineEditor {
public:
    LineEditor() = default;
    virtual ~LineEditor() = default;
    LineEditor(const LineEditor &) = delete;
    LineEditor &operator=(const LineEditor &) = delete;
    LineEditor(LineEditor &&) = delete;
    LineEditor &operator=(LineEditor &&) = delete;

    [[nodiscard]]
    virtual std::optional<std::string> prompt(std::string_view prompt_text) = 0;
    virtual void append_history(std::string_view line) = 0;
};

class ReplxxLineEditor final : public LineEditor {
public:
    ReplxxLineEditor();
    ~ReplxxLineEditor() override;
    ReplxxLineEditor(const ReplxxLineEditor &) = delete;
    ReplxxLineEditor &operator=(const ReplxxLineEditor &) = delete;
    ReplxxLineEditor(ReplxxLineEditor &&) = delete;
    ReplxxLineEditor &operator=(ReplxxLineEditor &&) = delete;

    [[nodiscard]]
    std::optional<std::string> prompt(std::string_view prompt_text) override;
    void append_history(std::string_view line) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]]
std::optional<std::string> read_repl_input(LineEditor &editor);
[[nodiscard]]
std::string read_repl_multiline(LineEditor &editor, std::ostream &output);

} // namespace orangutan::app
