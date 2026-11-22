// bench/storage/main.cpp — registers and runs the oran-storage nanobench scenarios.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <print>

namespace orangutan::bench {
void register_audit_repository(ankerl::nanobench::Bench&);
void register_migrations(ankerl::nanobench::Bench&);
void register_pool_acquire(ankerl::nanobench::Bench&);
void register_pool_statement_cache(ankerl::nanobench::Bench&);
void register_session_repository(ankerl::nanobench::Bench&);
void register_sqlite_insert(ankerl::nanobench::Bench&);
void register_statement_cache(ankerl::nanobench::Bench&);
void register_trace_repository(ankerl::nanobench::Bench&);
void register_trace_turn_insert(ankerl::nanobench::Bench&);
}  // namespace orangutan::bench

int main() {
  ankerl::nanobench::Bench b;
  b.title("bench-storage");
  b.unit("batch");
  b.minEpochIterations(1000);
  b.warmup(100);

  orangutan::bench::register_migrations(b);
  orangutan::bench::register_sqlite_insert(b);
  orangutan::bench::register_pool_acquire(b);
  orangutan::bench::register_statement_cache(b);
  orangutan::bench::register_pool_statement_cache(b);
  orangutan::bench::register_session_repository(b);
  orangutan::bench::register_audit_repository(b);
  orangutan::bench::register_trace_repository(b);
  orangutan::bench::register_trace_turn_insert(b);

  std::println();
  return 0;
}
