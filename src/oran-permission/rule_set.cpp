#include <oran/permission/rule_set.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <oran/core/capability.hpp>
#include <oran/core/enum_names.hpp>
#include <oran/permission/input_pattern.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] Verdict mode_default_verdict(Mode mode) noexcept {
  switch (mode) {
    case Mode::strict:
    case Mode::sandboxed:
      return Verdict::deny;
    case Mode::default_:
      return Verdict::ask;
    case Mode::permissive:
      return Verdict::allow;
  }
  return Verdict::deny;
}

[[nodiscard]] std::string format_reason(std::size_t index, const Rule& rule) {
  const auto verdict_name = core::enum_name(rule.verdict);
  std::string out;
  std::format_to(std::back_inserter(out), "rule #{} ({}: {}", index, verdict_name, rule.tool_pattern);
  if (rule.capability.has_value()) {
    std::format_to(std::back_inserter(out), " capability={}", core::enum_name(*rule.capability));
  }
  if (rule.input_pattern.has_value()) {
    std::format_to(std::back_inserter(out), " input=~\"{}\"", rule.input_pattern->pattern());
  }
  out.push_back(')');
  return out;
}

[[nodiscard]] Decision evaluate_requirement(std::span<const Rule> rules,
                                            std::string_view tool_name,
                                            std::string_view input,
                                            std::optional<core::Capability> capability,
                                            Mode mode) {
  for (const auto verdict : std::array{Verdict::deny, Verdict::allow, Verdict::ask}) {
    const auto match = std::ranges::find_if(rules, [&](const Rule& rule) {
      return rule.verdict == verdict && glob_match(rule.tool_pattern, tool_name) &&
             (!rule.capability || rule.capability == capability) &&
             (!rule.input_pattern || rule.input_pattern->matches(input));
    });
    if (match != rules.end()) {
      return Decision{
          .verdict = verdict,
          .reason = format_reason(static_cast<std::size_t>(match - rules.begin()), *match),
          .replay_max = match->replay_max,
          .approval_ttl = match->approval_ttl,
      };
    }
  }
  return Decision{
      .verdict = mode_default_verdict(mode),
      .reason = std::format("default by mode={}", core::enum_name(mode)),
  };
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
  // Iterative two-pointer glob matcher with backtracking on `*`. Standard
  // textbook algorithm: O(|pattern| + |text|) in the common case and
  // O(|pattern| * |text|) worst case for adversarial inputs (an acceptable
  // cost at the size of a tool name).
  std::size_t pi = 0;
  std::size_t ti = 0;
  std::size_t star = std::string_view::npos;
  std::size_t match = 0;
  while (ti < text.size()) {
    if (pi < pattern.size() && pattern[pi] == '*') {
      star = pi++;
      match = ti;
    } else if (pi < pattern.size() && pattern[pi] == text[ti]) {
      ++pi;
      ++ti;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      ++match;
      ti = match;
    } else {
      return false;
    }
  }
  while (pi < pattern.size() && pattern[pi] == '*') {
    ++pi;
  }
  return pi == pattern.size();
}

Decision evaluate(std::span<const Rule> rules, std::string_view tool_name, Mode mode) {
  return evaluate(rules, tool_name, std::string_view{}, std::span<const core::Capability>{}, mode);
}

Decision intersect(Decision left, Decision right) {
  if (left.verdict == Verdict::deny) {
    return left;
  }
  if (right.verdict == Verdict::deny) {
    return right;
  }
  if (right.verdict == Verdict::ask) {
    if (left.verdict == Verdict::ask) {
      left.replay_max = std::min(left.replay_max, right.replay_max);
      left.approval_ttl = std::min(left.approval_ttl, right.approval_ttl);
    } else {
      return right;
    }
  }
  return left;
}

Decision evaluate(std::span<const Rule> rules,
                  std::string_view tool_name,
                  std::span<const core::Capability> required_capabilities,
                  Mode mode) {
  return evaluate(rules, tool_name, std::string_view{}, required_capabilities, mode);
}

Decision evaluate(std::span<const Rule> rules,
                  std::string_view tool_name,
                  std::string_view input,
                  std::span<const core::Capability> required_capabilities,
                  Mode mode) {
  if (required_capabilities.empty()) {
    return evaluate_requirement(rules, tool_name, input, std::nullopt, mode);
  }

  auto combined = evaluate_requirement(rules, tool_name, input, required_capabilities.front(), mode);
  for (const auto capability : required_capabilities.subspan(1)) {
    if (combined.verdict == Verdict::deny) {
      return combined;
    }
    combined = intersect(std::move(combined), evaluate_requirement(rules, tool_name, input, capability, mode));
  }
  return combined;
}

}  // namespace orangutan::permission
