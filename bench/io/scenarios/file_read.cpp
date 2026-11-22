// bench/io/scenarios/file_read.cpp
//
// A-vs-B comparison: direct blocking text read vs. the oran-io coroutine wrapper.

#include <nanobench.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <oran/io.hpp>

namespace orangutan::bench {

namespace {

class TempFile {
public:
  TempFile()
      : path_(
            std::filesystem::temp_directory_path() /
            ("oran-io-bench-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")) {
    std::ofstream output{path_, std::ios::binary};
    for (int i = 0; i < 64; ++i) {
      output << "orangutan v2 io benchmark line " << i << '\n';
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

[[gnu::noinline]] std::size_t direct_read(std::string_view path) {
  std::ifstream input{std::filesystem::path{path}, std::ios::binary};
  std::string contents{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
  return contents.size();
}

[[gnu::noinline]] std::size_t coroutine_read(std::string_view path) {
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

}  // namespace

void register_file_read(ankerl::nanobench::Bench& bench) {
  TempFile file;
  const auto path = file.path_string();

  bench.run("io.direct_read", [&] {
    auto size = direct_read(path);
    ankerl::nanobench::doNotOptimizeAway(size);
  });
  bench.run("io.coroutine_read_text_file", [&] {
    auto size = coroutine_read(path);
    ankerl::nanobench::doNotOptimizeAway(size);
  });
}

}  // namespace orangutan::bench
