// tests/io/test_fingerprint.cpp — FileFingerprint coverage.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <oran/core/error.hpp>
#include <oran/io.hpp>

namespace core = orangutan::core;
namespace io = orangutan::io;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_(std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] std::filesystem::path path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void touch_mtime(const std::filesystem::path& path) {
  auto current = std::filesystem::last_write_time(path);
  // Bump well past the platform's mtime resolution (typically 1 ns on ext4
  // / xfs, 1 µs on tmpfs, 1 ms on FAT) so the second fingerprint cannot
  // collide with the first.
  std::filesystem::last_write_time(path, current + std::chrono::seconds{1});
}

}  // namespace

TEST_CASE("compute_file_fingerprint returns size and mtime for a regular file", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-basic"};
  const auto file = temp.path() / "note.txt";
  write_direct(file, "hello orangutan");

  auto fp = io::compute_file_fingerprint(file.string());
  REQUIRE(fp.has_value());
  REQUIRE(fp->size_bytes == 15);
  REQUIRE(fp->mtime_ns > 0);
  REQUIRE_FALSE(fp->sha256.has_value());
}

TEST_CASE("compute_file_fingerprint is stable across reads", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-stable"};
  const auto file = temp.path() / "note.txt";
  write_direct(file, "stable");

  auto first = io::compute_file_fingerprint(file.string());
  auto second = io::compute_file_fingerprint(file.string());
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  REQUIRE(*first == *second);
}

TEST_CASE("compute_file_fingerprint changes when mtime moves", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-mtime"};
  const auto file = temp.path() / "note.txt";
  write_direct(file, "v1");

  auto before = io::compute_file_fingerprint(file.string());
  REQUIRE(before.has_value());
  touch_mtime(file);
  auto after = io::compute_file_fingerprint(file.string());
  REQUIRE(after.has_value());
  REQUIRE(before->size_bytes == after->size_bytes);
  REQUIRE(before->mtime_ns != after->mtime_ns);
}

TEST_CASE("compute_file_fingerprint changes when content size changes", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-resize"};
  const auto file = temp.path() / "note.txt";
  write_direct(file, "short");
  auto before = io::compute_file_fingerprint(file.string());
  REQUIRE(before.has_value());

  write_direct(file, "noticeably longer body");
  auto after = io::compute_file_fingerprint(file.string());
  REQUIRE(after.has_value());
  REQUIRE(before->size_bytes < after->size_bytes);
}

TEST_CASE("compute_file_fingerprint reports missing files as not_found", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-missing"};
  auto fp = io::compute_file_fingerprint((temp.path() / "absent.txt").string());
  REQUIRE_FALSE(fp.has_value());
  REQUIRE(fp.error().kind() == core::ErrorKind::not_found);
}

TEST_CASE("compute_file_fingerprint refuses directories", "[unit][io][fingerprint]") {
  TempDir temp{"oran-io-fingerprint-dir"};
  auto fp = io::compute_file_fingerprint(temp.path().string());
  REQUIRE_FALSE(fp.has_value());
  REQUIRE(fp.error().kind() == core::ErrorKind::io);
}

TEST_CASE("compute_file_fingerprint rejects empty paths", "[unit][io][fingerprint]") {
  auto fp = io::compute_file_fingerprint("");
  REQUIRE_FALSE(fp.has_value());
  REQUIRE(fp.error().kind() == core::ErrorKind::invalid_argument);
}
