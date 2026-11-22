#include <oran/io/private_directory.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <oran/core/turn_id.hpp>

namespace {
namespace io = orangutan::io;
namespace core = orangutan::core;

class Directory {
public:
  Directory() {
    auto id = core::generate_turn_id();
    REQUIRE(id);
    path = std::filesystem::temp_directory_path() / ("oran-private-" + core::format_turn_id_hex(*id));
  }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};
}  // namespace

TEST_CASE("private application files survive replacement with owner-only permissions", "[io][private-directory]") {
  Directory fixture;
  {
    auto directory = io::PrivateDirectory::open(fixture.path.string());
    REQUIRE(directory);
    REQUIRE(directory->write("settings.json", "first credential"));
    REQUIRE(directory->write("settings.json", "replacement credential"));
  }
  auto reopened = io::PrivateDirectory::open(fixture.path.string());
  REQUIRE(reopened);
  auto contents = reopened->read("settings.json", 1024);
  REQUIRE(contents);
  REQUIRE(contents->has_value());
  CHECK(**contents == "replacement credential");
  struct stat status{};
  REQUIRE(::stat((fixture.path / "settings.json").c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0600);
  REQUIRE(::stat(fixture.path.c_str(), &status) == 0);
  CHECK((status.st_mode & 0777) == 0700);
}

TEST_CASE("private file reads reject symlinks and oversized content", "[io][private-directory]") {
  Directory fixture;
  auto directory = io::PrivateDirectory::open(fixture.path.string());
  REQUIRE(directory);
  REQUIRE(directory->write("secret", "confidential"));
  std::filesystem::create_symlink("secret", fixture.path / "redirect");

  auto symlink = directory->read("redirect", 1024);
  auto oversized = directory->read("secret", 3);

  REQUIRE_FALSE(symlink);
  CHECK(symlink.error().kind() == core::ErrorKind::io);
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error().kind() == core::ErrorKind::invalid_argument);
}

TEST_CASE("private file operations reject paths outside their directory", "[io][private-directory]") {
  Directory fixture;
  auto directory = io::PrivateDirectory::open((fixture.path / "owned").string());
  REQUIRE(directory);

  auto escaped = directory->write("../escape", "must stay private");

  REQUIRE_FALSE(escaped);
  CHECK(escaped.error().kind() == core::ErrorKind::invalid_argument);
  CHECK_FALSE(std::filesystem::exists(fixture.path / "escape"));
}

TEST_CASE("an application state lock excludes another owner until release", "[io][private-directory]") {
  Directory fixture;
  auto directory = io::PrivateDirectory::open(fixture.path.string());
  REQUIRE(directory);
  {
    auto owner = directory->lock("service.lock");
    REQUIRE(owner);
    auto other = directory->lock("service.lock");
    REQUIRE_FALSE(other);
    CHECK(other.error().kind() == core::ErrorKind::conflict);
  }
  auto successor = directory->lock("service.lock");
  REQUIRE(successor);
}
