// bench/tool/scenarios/file_edit.cpp
//
// A-vs-B comparison: full `FileEdit` dispatch through the registry under
// `unique_replace` (one match, `replace_all` omitted) vs. `replace_all_many`
// (many matches, `replace_all=true`). Both scenarios pay the same fixed
// costs (permission eval + libsodium SHA-256 of the input + audit record +
// JSON parse + a 1 KiB read + a 1 KiB write); the contrast measures whether
// rebuilding the contents with N substitutions is materially more expensive
// than a single substitution at typical edit sizes. Useful as the baseline
// a future "rope" or "in-place rewrite" optimization would have to beat.

#include <nanobench.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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
              ("oran-tool-bench-edit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
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
        auto result = co_await registry.dispatch(tool::kFileEditName, input, ctx);
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

/// 1 KiB seed for the unique-replace case: one occurrence of "NEEDLE"
/// surrounded by alphabetic filler that does not contain the marker.
std::string make_unique_seed() {
  std::string seed(1024, 'x');
  static constexpr std::string_view kMarker = "NEEDLE";
  seed.replace(512, kMarker.size(), kMarker);
  return seed;
}

/// 1 KiB seed for the replace_all case: 64 occurrences of "LOOP", one every
/// 16 bytes, surrounded by filler that does not contain the marker.
std::string make_replace_all_seed() {
  std::string seed(1024, 'x');
  for (std::size_t offset = 0; offset + 4 <= seed.size(); offset += 16) {
    seed.replace(offset, 4, "LOOP");
  }
  return seed;
}

}  // namespace

void register_tool_file_edit(ankerl::nanobench::Bench& bench) {
  tool::Registry registry;
  if (!tool::register_file_edit(registry)) {
    std::abort();
  }

  permission::RuleSet rules;
  rules.push_back(permission::Rule{
      .verdict = permission::Verdict::allow,
      .tool_pattern = std::string{tool::kFileEditName},
      .capability = core::Capability::edit_file,
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
  const auto path = file.path_string();
  const auto unique_seed = make_unique_seed();
  const auto replace_all_seed = make_replace_all_seed();
  const std::string unique_input = R"({"path":")" + path + R"(","old_string":"NEEDLE","new_string":"FOUND!"})";
  const std::string replace_all_input =
      R"({"path":")" + path + R"(","old_string":"LOOP","new_string":"DONE","replace_all":true})";

  bench.run("file_edit.dispatch_unique_replace", [&] {
    sink.clear();
    file.seed(unique_seed);
    const auto bytes = run_dispatch(io, registry, ctx, unique_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
  bench.run("file_edit.dispatch_replace_all_many", [&] {
    sink.clear();
    file.seed(replace_all_seed);
    const auto bytes = run_dispatch(io, registry, ctx, replace_all_input);
    ankerl::nanobench::doNotOptimizeAway(bytes);
  });
}

}  // namespace orangutan::bench
