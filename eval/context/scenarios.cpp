#include "evaluation.hpp"

#include <algorithm>
#include <format>
#include <map>

#include <oran/core/error.hpp>

namespace orangutan::evaluation {
namespace {
std::string activity(int index) {
  return std::format("Reviewed fixture shard {}: 17 quoted fields, 3 empty cells, 2 trailing delimiters. "
                     "The sample includes escaped quotes, UTF-8 labels and CRLF lines. This is an inspection "
                     "record only; it does not change the task, constraints, decision or completion status.",
                     index);
}
}  // namespace

std::vector<Scenario> scenarios() {
  std::vector<Scenario> cases;
  for (const auto* name : {"goal_constraints", "superseded_decision", "completed_vs_pending", "large_tool_result"}) {
    Scenario scenario{.name = name,
                      .expected = {{"goal", "repair_csv_import"},
                                   {"constraints", {"no_new_dependencies", "preserve_user_records"}},
                                   {"decision", "streaming_parser"},
                                   {"completed", {"inspect_parser"}},
                                   {"pending", {"regression_tests", "release"}}}};
    const bool corrected = scenario.name == "superseded_decision";
    scenario.large_tool = scenario.name == "large_tool_result";
    scenario.obsolete_decision = corrected ? "rewrite_parser" : "";
    scenario.history.push_back(core::Message::user_text(
        "Our active task ID is repair_csv_import. Constraints: no_new_dependencies and preserve_user_records. "
        "Use these exact labels in handoff JSON. Only inspect_parser is complete. regression_tests and release "
        "are pending; proposing either is not evidence of completion. The implementation decision is " +
        std::string{corrected ? "rewrite_parser" : "streaming_parser"} + "."));
    scenario.history.push_back(
        core::Message::assistant_text("I will inspect the import path under those constraints."));
    for (int i = 0; i < 79; ++i) {
      if (corrected && i == 30) {
        scenario.history.push_back(core::Message::user_text("Correction: reject rewrite_parser. The current decision "
                                                            "is streaming_parser. Preserve all other task state."));
        scenario.history.push_back(
            core::Message::assistant_text("Acknowledged: streaming_parser supersedes the old decision."));
      } else if (scenario.name == "completed_vs_pending" && i == 30) {
        scenario.history.push_back(core::Message::user_text(
            "The compile check has now succeeded: add compile to completed. regression_tests have NOT run. "
            "release is still pending. Do not confuse the earlier plan to run tests with a result."));
        scenario.history.push_back(
            core::Message::assistant_text("Compile succeeded; tests and release remain pending."));
        scenario.expected["completed"].push_back("compile");
      } else {
        scenario.history.push_back(
            core::Message::user_text("Record this inspection without changing the active task: " + activity(i)));
        scenario.history.push_back(core::Message::assistant_text(activity(i)));
      }
    }
    if (scenario.large_tool)
      scenario.expected["tool_marker"] = "fixture_schema_v2";
    cases.push_back(std::move(scenario));
  }
  return cases;
}

nlohmann::json grade(std::string_view answer, const Scenario& scenario) {
  using nlohmann::json;
  // Accept an ordinary fenced JSON answer but no trailing prose or coercion.
  if (answer.starts_with("```json\n") && answer.find_last_not_of(" \r\n") != std::string_view::npos) {
    answer.remove_prefix(8);
    const auto end = answer.rfind("```");
    if (end != std::string_view::npos && answer.substr(end + 3).find_first_not_of(" \r\n") == std::string_view::npos)
      answer = answer.substr(0, end);
  }
  auto parsed = json::parse(answer, nullptr, false);
  json checks = json::object();
  bool passed = parsed.is_object();
  for (const auto& [key, expected] : scenario.expected.items()) {
    bool match = parsed.is_object() && parsed.contains(key);
    if (match && expected.is_array() && parsed[key].is_array()) {
      auto wanted = expected;
      auto actual = parsed[key];
      std::ranges::sort(wanted.get_ref<json::array_t&>());
      std::ranges::sort(actual.get_ref<json::array_t&>());
      match = wanted == actual;
    } else if (match)
      match = parsed[key] == expected;
    checks[key] = match;
    passed = passed && match;
  }
  bool false_completion = false;
  if (parsed.is_object() && parsed.contains("completed") && parsed["completed"].is_array())
    for (const auto& pending : scenario.expected["pending"])
      false_completion = false_completion || std::ranges::contains(parsed["completed"], pending);
  return {{"passed", passed},
          {"valid_json", parsed.is_object()},
          {"checks", checks},
          {"lost_constraints", !checks.value("constraints", false)},
          {"stale_decision",
           !scenario.obsolete_decision.empty() && parsed.is_object() && parsed.contains("decision") &&
               parsed["decision"] == scenario.obsolete_decision},
          {"false_completion", false_completion},
          {"answer", parsed.is_discarded() ? json(nullptr) : parsed}};
}

void summarize(nlohmann::json& report) {
  using nlohmann::json;
  json totals = {{"phases", 0},
                 {"passed_phases", 0},
                 {"input_tokens", 0},
                 {"output_tokens", 0},
                 {"cache_read_tokens", 0},
                 {"cache_creation_tokens", 0},
                 {"lost_constraints", 0},
                 {"stale_decisions", 0},
                 {"false_completions", 0},
                 {"invalid_answers", 0},
                 {"operational_failures", 0},
                 {"graded_phases", 0},
                 {"paired_regressions", 0},
                 {"elapsed_ms", 0}};
  json variants = json::object();
  bool known = true;
  double cost = 0;
  std::map<std::string, bool> baselines;
  for (const auto& row : report.value("cases", json::array())) {
    const auto variant = row.value("variant", std::string{});
    if (!variants.contains(variant))
      variants[variant] = {{"phases", 0}, {"passed", 0}};
    for (const auto& phase : row["phases"]) {
      totals["phases"] = totals["phases"].get<int>() + 1;
      variants[variant]["phases"] = variants[variant]["phases"].get<int>() + 1;
      const bool passed = phase.value("passed", false);
      totals["passed_phases"] = totals["passed_phases"].get<int>() + static_cast<int>(passed);
      variants[variant]["passed"] = variants[variant]["passed"].get<int>() + static_cast<int>(passed);
      for (const auto* field :
           {"input_tokens", "output_tokens", "cache_read_tokens", "cache_creation_tokens", "elapsed_ms"})
        totals[field] = totals[field].get<std::uint64_t>() + phase.value(field, std::uint64_t{});
      const auto grading = phase.value("grading", json::object());
      totals["lost_constraints"] =
          totals["lost_constraints"].get<int>() + static_cast<int>(grading.value("lost_constraints", false));
      totals["stale_decisions"] =
          totals["stale_decisions"].get<int>() + static_cast<int>(grading.value("stale_decision", false));
      totals["false_completions"] =
          totals["false_completions"].get<int>() + static_cast<int>(grading.value("false_completion", false));
      const bool graded = grading.contains("valid_json");
      totals["graded_phases"] = totals["graded_phases"].get<int>() + static_cast<int>(graded);
      totals["operational_failures"] = totals["operational_failures"].get<int>() + static_cast<int>(!graded);
      totals["invalid_answers"] =
          totals["invalid_answers"].get<int>() + static_cast<int>(graded && !grading.value("valid_json", false));
      if (phase.contains("cost_estimate_usd") && phase["cost_estimate_usd"].is_number())
        cost += phase["cost_estimate_usd"].get<double>();
      else
        known = false;
      const auto key = row.value("scenario", std::string{}) + std::to_string(row.value("repeat", 0)) +
                       phase.value("phase", std::string{});
      if (variant == "baseline")
        baselines[key] = passed;
      else if (baselines.contains(key) && baselines.at(key) && !passed)
        totals["paired_regressions"] = totals["paired_regressions"].get<int>() + 1;
    }
  }
  totals["cost_estimate_usd"] = known && totals["phases"] != 0 ? json(cost) : json(nullptr);
  for (auto& value : variants)
    value["pass_rate"] = static_cast<double>(value["passed"].get<int>()) / value["phases"].get<int>();
  report["totals"] = std::move(totals);
  report["variants"] = std::move(variants);
}

core::Result<void> validate(const Options& options) {
  const auto cases = scenarios();
  if (options.output.empty() || options.repeats < 1 || options.repeats > 5 || options.max_calls < 1 ||
      options.max_calls > 512 || options.context_tokens < 8192 || options.context_tokens > 32768 ||
      (options.selected_case != "all" && !std::ranges::contains(cases, options.selected_case, &Scenario::name)))
    return std::unexpected(core::Error::invalid_argument("invalid evaluation options"));
  return {};
}
}  // namespace orangutan::evaluation
