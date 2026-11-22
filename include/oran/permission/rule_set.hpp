#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/permission/input_pattern.hpp>

namespace orangutan::permission {

enum class Verdict : std::uint8_t {
  allow,
  deny,
  ask,
};

enum class Mode : std::uint8_t {
  /// Deny everything not explicitly allowed.
  strict,
  /// Ask for unmatched effects.
  default_,
  /// Permissive baseline: unmatched calls allowed.
  permissive,
  /// Deny unmatched effects; the baseline permits reads.
  sandboxed,
};

struct Rule {
  Verdict verdict{Verdict::deny};
  /// Glob pattern matched against the tool name. `*` matches any (possibly
  /// empty) byte sequence; everything else matches literally. Examples:
  /// `FileRead`, `File*`, `ShellExec`.
  std::string tool_pattern;
  /// An absent scope matches the whole tool; a scoped rule matches that capability.
  std::optional<core::Capability> capability{};
  /// Optional runtime regex matched against the call's `input` string
  /// (re2 partial match). When unset, the rule does not constrain the
  /// input. When set, the rule matches only when the pattern accepts
  /// the input — `Rule` is move-only as a result (re2 is non-copyable).
  std::optional<InputPattern> input_pattern{};
  /// Replay budget and lifetime for an approved exact operation.
  std::uint32_t replay_max{8};
  std::chrono::seconds approval_ttl{3600};

  friend bool operator==(const Rule&, const Rule&) = default;
};

struct Decision {
  Verdict verdict{Verdict::deny};
  /// Human-readable explanation: which rule fired, or which mode fell back.
  /// When the firing rule had a capability scope, the spelling appears in
  /// the reason (e.g. `rule #2 (allow: File* capability=read_file)`).
  /// When the firing rule had an input_pattern, the pattern source string
  /// appears too (e.g. `rule #3 (deny: ShellExec input=~"^rm ")`).
  std::string reason;
  std::uint32_t replay_max{8};
  std::chrono::seconds approval_ttl{3600};

  friend bool operator==(const Decision&, const Decision&) = default;
};

using RuleSet = std::vector<Rule>;

/// Borrows immutable rules for the lifetime of an execution boundary.
struct PolicyView {
  std::span<const Rule> rules;
  Mode mode{Mode::strict};
};

/// Intersect authority: deny dominates ask, and asks retain the smaller limits.
[[nodiscard]] Decision intersect(Decision left, Decision right);

/// Evaluate every required capability without effects. Deny outranks ask,
/// which outranks allow across capabilities; ask limits intersect.
[[nodiscard]] Decision evaluate(std::span<const Rule> rules, std::string_view tool_name, Mode mode);
[[nodiscard]] Decision evaluate(std::span<const Rule> rules,
                                std::string_view tool_name,
                                std::span<const core::Capability> required_capabilities,
                                Mode mode);
[[nodiscard]] Decision evaluate(std::span<const Rule> rules,
                                std::string_view tool_name,
                                std::string_view input,
                                std::span<const core::Capability> required_capabilities,
                                Mode mode);

/// True iff `pattern` (a `*`-glob) matches `text` byte-for-byte. Exposed so
/// callers / tests can reach for the same matcher the rule set uses.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text) noexcept;

}  // namespace orangutan::permission

template <>
struct std::formatter<orangutan::permission::Verdict> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::Verdict v, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(v), ctx);
  }
};

template <>
struct std::formatter<orangutan::permission::Mode> : std::formatter<std::string_view> {
  template <class FormatContext>
  auto format(orangutan::permission::Mode m, FormatContext& ctx) const {
    return std::formatter<std::string_view>::format(orangutan::core::enum_name(m), ctx);
  }
};
