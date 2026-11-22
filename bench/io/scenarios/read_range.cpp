// bench/io/scenarios/read_range.cpp
//
// Spec 0011 v1 validation hint: "10 MiB source file, line range 120-199".
// The A/B compares a whole-file `read_text_file` against a line-range
// `read_text_file_ranged` over a 10 MiB synthetic source. The range
// variant should beat the whole-file read because it returns only the
// requested span — but the mid-read fingerprint capture pair shows up as
// a small fixed overhead in both cases.

#include <nanobench.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/io.hpp>

namespace orangutan::bench {

namespace {

class TempFile {
public:
  explicit TempFile(std::size_t line_count)
      : path_(std::filesystem::temp_directory_path() /
              ("oran-io-bench-range-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               ".txt")) {
    std::ofstream output{path_, std::ios::binary};
    for (std::size_t i = 0; i < line_count; ++i) {
      output << "orangutan v2 io range-read scenario bench line " << i << '\n';
    }
  }

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  [[nodiscard]] std::string path_string() const {
    return path_.string();
  }

private:
  std::filesystem::path path_;
};

[[gnu::noinline]] std::size_t whole_read(std::string_view path) {
  asio::io_context context;
  std::size_t size = 0;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<void> {
        auto result = co_await io::read_text_file(context.get_executor(), std::string{path});
        if (result) {
          size = result->size();
        }
        context.stop();
        co_return;
      },
      asio::detached);

  context.run();
  return size;
}

[[gnu::noinline]] std::size_t ranged_read(std::string_view path) {
  asio::io_context context;
  std::size_t size = 0;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<void> {
        io::ReadTextOptions options;
        options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 120, .line_count = 80}};
        auto result = co_await io::read_text_file_ranged(context.get_executor(), std::string{path}, options);
        if (result) {
          size = result->text.size();
        }
        context.stop();
        co_return;
      },
      asio::detached);

  context.run();
  return size;
}

}  // namespace

void register_read_range(ankerl::nanobench::Bench& bench) {
  // ~10 MiB at ~60 bytes per line -> ~175 000 lines.
  TempFile file{175'000U};
  const auto path = file.path_string();

  bench.run("io.read_text_file_whole_10mib", [&] {
    auto size = whole_read(path);
    ankerl::nanobench::doNotOptimizeAway(size);
  });

  bench.run("io.read_text_file_ranged_lines_80", [&] {
    auto size = ranged_read(path);
    ankerl::nanobench::doNotOptimizeAway(size);
  });
}

}  // namespace orangutan::bench
