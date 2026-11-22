// src/oran-permission/input_pattern.cpp — `InputPattern` implementation.
//
// re2 lives only here (rule C6 in `docs/rules/critical-rules.md`). The
// public header forward-declares `re2::RE2` and keeps the rest of the
// project free of re2's headers and abseil transitive cost.

#include <oran/permission/input_pattern.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <re2/re2.h>

#include <oran/core/error.hpp>
#include <oran/core/result.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] re2::RE2::Options quiet_options() noexcept {
  re2::RE2::Options options{re2::RE2::DefaultOptions};
  options.set_log_errors(false);
  return options;
}

}  // namespace

core::Result<InputPattern> InputPattern::compile(std::string pattern) {
  auto re = std::make_unique<re2::RE2>(pattern, quiet_options());
  if (!re->ok()) {
    return std::unexpected(core::Error::invalid_argument("invalid regex").with("regex_error", re->error()));
  }
  return InputPattern{std::move(pattern), std::move(re)};
}

bool InputPattern::matches(std::string_view input) const noexcept {
  return re2::RE2::PartialMatch(input, *re_);
}

InputPattern::InputPattern(std::string pattern, std::unique_ptr<re2::RE2> re) noexcept
    : pattern_(std::move(pattern)), re_(std::move(re)) {}

InputPattern::InputPattern(InputPattern&&) noexcept = default;
InputPattern& InputPattern::operator=(InputPattern&&) noexcept = default;
InputPattern::~InputPattern() = default;

}  // namespace orangutan::permission
