// bench/bootstrap/scenarios/runtime_assembly_build.cpp
//
// A-vs-B: cost of `RuntimeAssembly::build` with the storage-backed audit
// pipeline (Pool + migration + StorageAuditSink + ApprovalBroker) vs. the
// no-audit fast path (NullAuditSink + ApprovalBroker only). The win shape
// here is "how much does provisioning audit cost per process?" — useful
// to keep visible as the assembly grows.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <asio/io_context.hpp>

#include <oran/bootstrap.hpp>

namespace orangutan::bench {
namespace bootstrap = orangutan::bootstrap;

namespace {

class WorkspaceFixture {
public:
  WorkspaceFixture()
      : root_(std::filesystem::temp_directory_path() /
              ("oran-assembly-bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(root_);
  }

  ~WorkspaceFixture() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  WorkspaceFixture(const WorkspaceFixture&) = delete;
  WorkspaceFixture& operator=(const WorkspaceFixture&) = delete;

  [[nodiscard]] std::string string() const {
    return root_.string();
  }

private:
  std::filesystem::path root_;
};

[[gnu::noinline]] std::size_t build_with_audit(const std::string& workspace, asio::io_context& io) {
  auto built = bootstrap::RuntimeAssembly::build(workspace, io.get_executor(), bootstrap::RuntimeAssemblyOptions{});
  if (!built) {
    std::abort();
  }
  return built->audit_path().size();
}

[[gnu::noinline]] std::size_t build_without_audit(const std::string& workspace, asio::io_context& io) {
  auto built = bootstrap::RuntimeAssembly::build(workspace,
                                                 io.get_executor(),
                                                 bootstrap::RuntimeAssemblyOptions{.audit_enabled = false});
  if (!built) {
    std::abort();
  }
  return built->audit_path().size() + 1U;
}

}  // namespace

void register_runtime_assembly_build(ankerl::nanobench::Bench& bench) {
  WorkspaceFixture audit_fixture;
  WorkspaceFixture null_fixture;
  asio::io_context io;
  const auto audit_workspace = audit_fixture.string();
  const auto null_workspace = null_fixture.string();

  // SQLite migration + Pool open dominates each iteration (~ms-scale);
  // cap iteration count low enough to keep the run under a few seconds.
  bench.minEpochIterations(500);
  bench.warmup(2);

  bench.run("bootstrap.assembly_build_with_audit", [&] {
    const auto v = build_with_audit(audit_workspace, io);
    ankerl::nanobench::doNotOptimizeAway(v);
  });
  bench.run("bootstrap.assembly_build_without_audit", [&] {
    const auto v = build_without_audit(null_workspace, io);
    ankerl::nanobench::doNotOptimizeAway(v);
  });
}

}  // namespace orangutan::bench
