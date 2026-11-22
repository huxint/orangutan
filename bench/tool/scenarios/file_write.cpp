// bench/tool/scenarios/file_write.cpp
//
// A-vs-B comparison: full `FileWrite` dispatch through the registry under
// `mode=truncate` vs. `mode=append`. Both scenarios pay the same costs
// (permission eval + libsodium SHA-256 of the input + audit record + JSON
// parse + a 64-byte write to a tempfile); the contrast measures whether
// the IO mode choice has any measurable effect at typical small-payload
// sizes. Useful as the baseline a future "buffered vs. unbuffered" or
// "direct vs. cached" write optimization would have to beat.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/capability.hpp>
#include <oran/core/result.hpp>
#include <oran/permission/audit.hpp>
#include <oran/permission/rule_set.hpp>
#include <oran/tool/builtins.hpp>
#include <oran/tool/registry.hpp>
#include <oran/tool/workspace.hpp>

namespace orangutan::bench {

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace permission = orangutan::permission;
namespace tool = orangutan::tool;

namespace {

class TempFile {
public:
  TempFile()
      : path_(std::filesystem::temp_directory_path() /
              ("oran-tool-bench-write-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".txt")) {}

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  void seed(std::string_view contents) const {
    std::ofstream output{path_, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  }

  void clear() const {
    seed("");
  }

  [[nodiscard]] std::string path_string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

[[gnu::noinline]] std::size_t
run_dispatch(asio::io_context& io, tool::Registry& registry, tool::DispatchContext& ctx, const std::string& input) {
  std::size_t bytes = 0;
  asio::co_spawn(
      io,
      [&]() -> async::Awaitable<void> {
        auto result = co_await registry.dispatch(tool::kFileWriteName, input, ctx);
        if (!result) {
          std::abort();
        }
        bytes += result->text.size();
        co_return;
      },
      asio::detached);
  io.run();
  io.restart();
  return bytes;
}

}  // namespace

void register_tool_file_write(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  if (!tool::register_file_write(registry)) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileWriteName},
      .capability = core::Capability::write_file,
  });
  permission::RecordingAuditSink sink;

  auto workspace = tool::Workspace::create(std::filesystem::temp_directory_path().string());
  if (!workspace) {
    std::abort();
  }

  asio::io_context io;
  tool::DispatchContext ctx{
      .executor = io.get_executor(),
      .mode = permission::Mode::strict,
      .rules = rules,
      .audit = sink,
      .workspace = &*workspace,
      .scope_key = "scope-A",
      .agent_key = "bencher",
      .identity = "operator-1",
  };

  TempFile file;
  // 64-byte payload — long enough to dominate the JSON parse cost but small
  // enough that the disk write is realistic for ordinary tool output.
  const std::string payload(64, 'x');
  const auto path = file.path_string();
  const std::string truncate_input = R"({"path":")" + path + R"(","content":")" + payload + R"(","mode":"truncate"})";
  const std::string append_input = R"({"path":")" + path + R"(","content":")" + payload + R"(","mode":"append"})";

  bench.run("file_write.dispatch_truncate", [&] {
    sink.clear();
    file.clear();
    const auto bytes = run_dispatch(io, registry, ctx, truncate_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
  bench.run("file_write.dispatch_append", [&] {
    sink.clear();
    file.clear();
    const auto bytes = run_dispatch(io, registry, ctx, append_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
}

}  // namespace orangutan::bench
