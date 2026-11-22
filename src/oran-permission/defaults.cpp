// src/oran-permission/defaults.cpp — `default_rules` baseline factory.

#include <oran/permission/defaults.hpp>

#include <oran/core/capability.hpp>
#include <oran/permission/rule_set.hpp>

namespace orangutan::permission {

namespace {

[[nodiscard]] Rule allow(core::Capability capability) {
  return Rule{
      .verdict = Verdict::allow,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] Rule ask(core::Capability capability) {
  return Rule{
      .verdict = Verdict::ask,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] Rule deny(core::Capability capability) {
  return Rule{
      .verdict = Verdict::deny,
      .tool_pattern = "*",
      .capability = capability,
      .input_pattern = std::nullopt,
  };
}

[[nodiscard]] RuleSet default_baseline() {
  RuleSet rs;
  rs.push_back(deny(core::Capability::runtime_loader));
  rs.push_back(deny(core::Capability::delete_path));
  rs.push_back(allow(core::Capability::read_file));
  rs.push_back(ask(core::Capability::write_file));
  rs.push_back(ask(core::Capability::edit_file));
  rs.push_back(ask(core::Capability::spawn_subprocess));
  rs.push_back(ask(core::Capability::egress_http));
  return rs;
}

[[nodiscard]] RuleSet permissive_baseline() {
  RuleSet rs;
  rs.push_back(deny(core::Capability::runtime_loader));
  rs.push_back(deny(core::Capability::delete_path));
  return rs;
}

[[nodiscard]] RuleSet sandboxed_baseline() {
  RuleSet rs;
  rs.push_back(allow(core::Capability::read_file));
  return rs;
}

}  // namespace

RuleSet default_rules(Mode mode) {
  switch (mode) {
    case Mode::strict:
      return {};
    case Mode::default_:
      return default_baseline();
    case Mode::permissive:
      return permissive_baseline();
    case Mode::sandboxed:
      return sandboxed_baseline();
  }
  return RuleSet{};
}

}  // namespace orangutan::permission
