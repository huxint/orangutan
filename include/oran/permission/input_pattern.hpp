#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <oran/core/result.hpp>

namespace re2 {
class RE2;
}  // namespace re2

namespace orangutan::permission {

class InputPattern {
public:
  /// Compile `pattern` into a `re2::RE2`. On success returns the wrapped
  /// pattern; on failure returns `Error::invalid_argument` with the re2
  /// error message attached as `regex_error`. The compiler is configured
  /// quietly — invalid patterns do not log to stderr; the caller is
  /// expected to surface the returned error to the operator (config
  /// loaders attach the source path).
  [[nodiscard]] static core::Result<InputPattern> compile(std::string pattern);

  /// True iff the compiled regex matches a substring of `input`. Cheap
  /// (no allocation) on the success and failure path alike — re2's
  /// PartialMatch with no capture targets is the documented fast path.
  [[nodiscard]] bool matches(std::string_view input) const noexcept;

  /// The pattern string this `InputPattern` was compiled from. Useful
  /// for diagnostics and for `Rule::operator==`.
  [[nodiscard]] std::string_view pattern() const noexcept {
    return pattern_;
  }

  InputPattern(const InputPattern&) = delete;
  InputPattern& operator=(const InputPattern&) = delete;
  InputPattern(InputPattern&&) noexcept;
  InputPattern& operator=(InputPattern&&) noexcept;
  ~InputPattern();

  /// Equality on the source pattern string. See the file-level comment for
  /// the rationale (never call into re2 for equality).
  friend bool operator==(const InputPattern& lhs, const InputPattern& rhs) noexcept {
    return lhs.pattern_ == rhs.pattern_;
  }

private:
  InputPattern(std::string pattern, std::unique_ptr<re2::RE2> re) noexcept;

  std::string pattern_;
  std::unique_ptr<re2::RE2> re_;
};

}  // namespace orangutan::permission
