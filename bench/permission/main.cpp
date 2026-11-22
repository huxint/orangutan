// bench/permission/main.cpp — registers and runs oran-permission nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_rule_set_scenarios(ankerl::nanobench::Bench&);
void register_defaults_scenarios(ankerl::nanobench::Bench&);
void register_input_pattern_scenarios(ankerl::nanobench::Bench&);
void register_approval_secret_scenarios(ankerl::nanobench::Bench&);
void register_approval_scenarios(ankerl::nanobench::Bench&);
void register_approval_broker_scenarios(ankerl::nanobench::Bench&);
void register_audit_scenarios(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/rule_set");
    b.unit("evaluate");
    b.minEpochIterations(200'000);
    b.warmup(2'000);

    orangutan::bench::register_rule_set_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/defaults");
    b.unit("RuleSet");
    b.minEpochIterations(50'000);
    b.warmup(1'000);

    orangutan::bench::register_defaults_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/input_pattern");
    b.unit("evaluate");
    b.minEpochIterations(100'000);
    b.warmup(1'000);

    orangutan::bench::register_input_pattern_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/approval_secret");
    b.unit("hmac");
    b.minEpochIterations(50'000);
    b.warmup(1'000);

    orangutan::bench::register_approval_secret_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/approval");
    b.unit("op");
    b.minEpochIterations(20'000);
    b.warmup(1'000);

    orangutan::bench::register_approval_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/approval_broker");
    b.unit("op");
    b.minEpochIterations(20'000);
    b.warmup(1'000);

    orangutan::bench::register_approval_broker_scenarios(b);
  }

  {
    ankerl::nanobench::Bench b;
    b.title("bench-permission/audit");
    b.unit("op");
    b.minEpochIterations(5'000);
    b.warmup(500);

    orangutan::bench::register_audit_scenarios(b);
  }

  std::println();
  return 0;
}
