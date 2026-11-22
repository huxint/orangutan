// bench/io/scenarios/fingerprint.cpp
//
// A-vs-B comparison: hand-rolled stat-style metadata read vs.
// `io::compute_file_fingerprint`. The two should produce the same numbers
// in roughly the same wall time; the helper just packages them into a
// stable `FileFingerprint` struct.

#include <nanobench.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <oran/io.hpp>

namespace orangutan::bench {

namespace {

class TempFile {
public:
  TempFile()
      : path_(std::filesystem::temp_directory_path() /
              ("oran-io-bench-fingerprint-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt")) {
    std::ofstream output{path_, std::ios::binary};
    for (int i = 0; i < 64; ++i) {
      output << "orangutan v2 io fingerprint scenario line " << i << '\n';
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

[[gnu::noinline]] std::uintmax_t direct_stat(std::string_view path) {
  const auto fs_path = std::filesystem::path{std::string{path}};
  std::error_code ec;
  const auto size = std::filesystem::file_size(fs_path, ec);
  const auto mtime = std::filesystem::last_write_time(fs_path, ec);
  return size + static_cast<std::uintmax_t>(mtime.time_since_epoch().count());
}

[[gnu::noinline]] std::uintmax_t helper_call(std::string_view path) {
  auto fp = io::compute_file_fingerprint(path);
  if (!fp) {
    return 0;
  }
  return fp->size_bytes + static_cast<std::uintmax_t>(fp->mtime_ns);
}

}  // namespace

void register_fingerprint(ankerl::nanobench::Bench& bench) {
  TempFile file;
  const auto path = file.path_string();

  bench.run("io.direct_stat_pair", [&] {
    auto value = direct_stat(path);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
  bench.run("io.compute_file_fingerprint", [&] {
    auto value = helper_call(path);
    ankerl::nanobench::doNotOptimizeAway(value);
  });
}

}  // namespace orangutan::bench
