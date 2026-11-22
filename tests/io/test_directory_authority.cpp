// tests/io/test_directory_authority.cpp — dirfd authority regressions.

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

#include <oran/io/directory_authority.hpp>
#include <oran/io/file.hpp>
#include <oran/io/fingerprint.hpp>

#include "../test-helpers/run_async.hpp"

namespace core = orangutan::core;
namespace io = orangutan::io;
namespace test = orangutan::tests;

namespace {

class TempDir {
public:
  explicit TempDir(std::string name)
      : path_{std::filesystem::temp_directory_path() /
              (std::move(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
    std::filesystem::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

[[nodiscard]] std::string read_direct(const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void require_no_mutation_temp(const std::filesystem::path& directory) {
  for (const auto& entry : std::filesystem::directory_iterator{directory}) {
    REQUIRE_FALSE(entry.path().filename().string().starts_with(".orangutan.tmp."));
  }
}

[[nodiscard]] std::string read_handle(const io::ReadOnlyFile& file) {
  std::array<char, 64> buffer{};
  const auto count = ::read(file.native_handle(), buffer.data(), buffer.size());
  REQUIRE(count >= 0);
  return std::string{buffer.data(), static_cast<std::size_t>(count)};
}

}  // namespace

TEST_CASE("DirectoryAuthority retains the opened root across pathname replacement", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace / "nested");
  std::filesystem::create_directories(outside / "nested");
  write_direct(workspace / "nested" / "note.txt", "inside");
  write_direct(outside / "nested" / "note.txt", "outside");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  auto opened = authority->open_file(io::AnchoredPath{
      .relative_path = "nested/note.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE(opened.has_value());
  REQUIRE(read_handle(*opened) == "inside");
  auto fingerprint = io::compute_file_fingerprint(*opened);
  REQUIRE(fingerprint.has_value());
  CHECK(fingerprint->size_bytes == 6);

  std::ifstream redirected{workspace / "nested" / "note.txt", std::ios::binary};
  std::string redirected_text;
  redirected >> redirected_text;
  REQUIRE(redirected_text == "outside");
}

TEST_CASE("DirectoryAuthority rejects traversal and symlink escapes", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-confinement"};
  const auto workspace = temp.path() / "workspace";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  write_direct(outside / "secret.txt", "secret");
  std::filesystem::create_directory_symlink(outside, workspace / "escape");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto traversal = authority->open_file(io::AnchoredPath{
      .relative_path = "../outside/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(traversal.has_value());
  REQUIRE(traversal.error().kind() == core::ErrorKind::permission_denied);

  auto absolute = authority->open_file(io::AnchoredPath{
      .relative_path = (outside / "secret.txt").string(),
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(absolute.has_value());
  REQUIRE(absolute.error().kind() == core::ErrorKind::invalid_argument);

  auto symlink_escape = authority->open_file(io::AnchoredPath{
      .relative_path = "escape/secret.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::allow_beneath,
  });
  REQUIRE_FALSE(symlink_escape.has_value());
  REQUIRE(symlink_escape.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("DirectoryAuthority reject-all policy refuses an in-root symlink", "[unit][io][authority]") {
  TempDir temp{"oran-io-authority-symlink-policy"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "real.txt", "inside");
  std::filesystem::create_symlink("real.txt", workspace / "link.txt");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto rejected = authority->open_file(io::AnchoredPath{
      .relative_path = "link.txt",
      .symlink_policy = io::AnchoredSymlinkPolicy::reject_all,
  });
  REQUIRE_FALSE(rejected.has_value());
  REQUIRE(rejected.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("FileMutation refuses symlink components and targets", "[unit][io][authority][mutation][symlink]") {
  TempDir temp{"oran-io-mutation-symlinks"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace / "real");
  write_direct(workspace / "real" / "note.txt", "inside");
  std::filesystem::create_directory_symlink("real", workspace / "parent-link");
  std::filesystem::create_symlink("real/note.txt", workspace / "target-link.txt");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());

  auto parent_symlink = authority->begin_file_mutation("parent-link/note.txt");
  REQUIRE_FALSE(parent_symlink.has_value());
  REQUIRE(parent_symlink.error().kind() == core::ErrorKind::permission_denied);

  auto target_symlink = authority->begin_file_mutation("target-link.txt");
  REQUIRE_FALSE(target_symlink.has_value());
  REQUIRE(target_symlink.error().kind() == core::ErrorKind::permission_denied);
}

TEST_CASE("FileMutation creates parent directories and appends", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-write"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "nested" / "output.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto created = authority->begin_file_mutation("nested/output.txt", true);
  REQUIRE(created.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto first = co_await io::write_text_file(context.get_executor(),
                                              std::move(*created),
                                              "one",
                                              io::WriteTextOptions{.atomic = true});
    REQUIRE(first.has_value());

    auto appended = authority->begin_file_mutation("nested/output.txt");
    REQUIRE(appended.has_value());
    auto second = co_await io::write_text_file(context.get_executor(),
                                               std::move(*appended),
                                               "\ntwo",
                                               io::WriteTextOptions{.mode = io::WriteMode::append});
    REQUIRE(second.has_value());
  });

  REQUIRE(read_direct(target) == "one\ntwo");
}

TEST_CASE("FileMutation refuses fail-if-exists overwrites", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-exclusive"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "output.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "existing");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("output.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new",
                                               io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::conflict);
  });

  REQUIRE(read_direct(target) == "existing");
}

TEST_CASE("FileMutation atomically replaces a file without leaving a temp", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-atomic"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);
  write_direct(target, "old contents");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());
  });

  REQUIRE(read_direct(target) == "new contents");
  require_no_mutation_temp(workspace);
}

TEST_CASE("FileMutation does not reuse legacy counter temp names", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-temp-name"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  const auto legacy_temp = workspace / ".data.txt.orangutan.tmp.0";
  std::filesystem::create_directory(workspace);
  write_direct(target, "old contents");
  write_direct(legacy_temp, "legacy marker");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "new contents",
                                               io::WriteTextOptions{.atomic = true});
    REQUIRE(result.has_value());
  });

  REQUIRE(read_direct(target) == "new contents");
  REQUIRE(read_direct(legacy_temp) == "legacy marker");
}

TEST_CASE("FileMutation supports atomic durability modes", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-durable"};
  const auto workspace = temp.path() / "workspace";
  const auto file_only = workspace / "file-only.txt";
  const auto file_and_parent = workspace / "file-and-parent.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto file_mutation = authority->begin_file_mutation("file-only.txt");
  auto parent_mutation = authority->begin_file_mutation("file-and-parent.txt");
  REQUIRE(file_mutation.has_value());
  REQUIRE(parent_mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto synced_file = co_await io::write_text_file(
        context.get_executor(),
        std::move(*file_mutation),
        "file durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file});
    REQUIRE(synced_file.has_value());

    auto synced_parent = co_await io::write_text_file(
        context.get_executor(),
        std::move(*parent_mutation),
        "parent durable",
        io::WriteTextOptions{.atomic = true, .durability = io::WriteTextDurability::fsync_file_and_parent});
    REQUIRE(synced_parent.has_value());
  });

  REQUIRE(read_direct(file_only) == "file durable");
  REQUIRE(read_direct(file_and_parent) == "parent durable");
}

TEST_CASE("FileMutation rejects durability without atomic mode before I/O", "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-durable-reject"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto result = co_await io::write_text_file(context.get_executor(),
                                               std::move(*mutation),
                                               "contents",
                                               io::WriteTextOptions{.durability = io::WriteTextDurability::fsync_file});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE_FALSE(std::filesystem::exists(target));
  require_no_mutation_temp(workspace);
}

TEST_CASE("FileMutation rejects atomic append and fail-if-exists before I/O",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-bad-mode"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "data.txt";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto append_mutation = authority->begin_file_mutation("data.txt");
  REQUIRE(append_mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto appended = co_await io::write_text_file(context.get_executor(),
                                                 std::move(*append_mutation),
                                                 "x",
                                                 io::WriteTextOptions{.mode = io::WriteMode::append, .atomic = true});
    REQUIRE_FALSE(appended.has_value());
    REQUIRE(appended.error().kind() == core::ErrorKind::invalid_argument);

    auto exclusive_mutation = authority->begin_file_mutation("data.txt");
    REQUIRE(exclusive_mutation.has_value());
    auto fail_if_exists =
        co_await io::write_text_file(context.get_executor(),
                                     std::move(*exclusive_mutation),
                                     "x",
                                     io::WriteTextOptions{.mode = io::WriteMode::fail_if_exists, .atomic = true});
    REQUIRE_FALSE(fail_if_exists.has_value());
    REQUIRE(fail_if_exists.error().kind() == core::ErrorKind::invalid_argument);
  });

  REQUIRE_FALSE(std::filesystem::exists(target));
  require_no_mutation_temp(workspace);
}

TEST_CASE("DirectoryAuthority pathname identity does not follow a final symlink", "[unit][io][authority][identity]") {
  TempDir temp{"oran-io-authority-path-identity"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  std::filesystem::create_directory(workspace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  REQUIRE(authority->refers_to_path(workspace.string()).value());

  std::filesystem::rename(workspace, moved_workspace);
  REQUIRE(authority->refers_to_path(moved_workspace.string()).value());
  REQUIRE_FALSE(authority->refers_to_path(workspace.string()).value());

  std::filesystem::create_directory_symlink(moved_workspace, workspace);
  REQUIRE_FALSE(authority->refers_to_path(workspace.string()).value());
}

TEST_CASE("FileMutation retains its parent authority across root replacement", "[unit][io][authority][mutation]") {
  TempDir temp{"oran-io-mutation-root-replacement"};
  const auto workspace = temp.path() / "workspace";
  const auto moved_workspace = temp.path() / "workspace-moved";
  const auto outside = temp.path() / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  write_direct(workspace / "note.txt", "inside-old");
  write_direct(outside / "note.txt", "outside-old");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  std::filesystem::rename(workspace, moved_workspace);
  std::filesystem::create_directory_symlink(outside, workspace);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "inside-new",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  std::ifstream inside{moved_workspace / "note.txt", std::ios::binary};
  std::ifstream outside_input{outside / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{inside}, std::istreambuf_iterator<char>{}} == "inside-new");
  REQUIRE(std::string{std::istreambuf_iterator<char>{outside_input}, std::istreambuf_iterator<char>{}} ==
          "outside-old");
}

TEST_CASE("FileMutation rejects a target change before commit and cleans its temp",
          "[unit][io][authority][mutation][conflict]") {
  TempDir temp{"oran-io-mutation-conflict"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "note.txt", "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  const auto replacement = workspace / "replacement.txt";
  write_direct(replacement, "external-version");
  std::filesystem::rename(replacement, workspace / "note.txt");

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "agent-version",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
  });

  std::ifstream current{workspace / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "external-version");
  for (const auto& entry : std::filesystem::directory_iterator{workspace}) {
    REQUIRE(entry.path().filename().string().find(".orangutan.tmp.") == std::string::npos);
  }
}

TEST_CASE("FileMutation detects same-size rewrites with restored mtime", "[unit][io][authority][mutation][conflict]") {
  TempDir temp{"oran-io-mutation-ctime-conflict"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  struct stat before{};
  REQUIRE(::stat(target.c_str(), &before) == 0);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  struct stat changed{};
  bool ctime_changed = false;
  for (std::size_t attempt = 0; attempt < 32U && !ctime_changed; ++attempt) {
    write_direct(target, "external");
    const timespec times[] = {before.st_atim, before.st_mtim};
    REQUIRE(::utimensat(AT_FDCWD, target.c_str(), times, 0) == 0);
    REQUIRE(::stat(target.c_str(), &changed) == 0);
    ctime_changed =
        changed.st_ctim.tv_sec != before.st_ctim.tv_sec || changed.st_ctim.tv_nsec != before.st_ctim.tv_nsec;
  }
  REQUIRE(ctime_changed);
  REQUIRE(changed.st_size == before.st_size);
  REQUIRE(changed.st_mtim.tv_sec == before.st_mtim.tv_sec);
  REQUIRE(changed.st_mtim.tv_nsec == before.st_mtim.tv_nsec);

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "agent-v",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "external");
}

TEST_CASE("FileMutation commit verifier aborts the atomic replacement and cleans its temp",
          "[unit][io][authority][mutation][atomic][conflict]") {
  TempDir temp{"oran-io-mutation-verifier-abort"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{.atomic = true};
    options.verify_before_commit = [](const io::FileFingerprint& fp) -> core::Result<void> {
      REQUIRE(fp.size_bytes == 8);
      REQUIRE(fp.mtime_ns > 0);
      return std::unexpected(core::Error{core::ErrorKind::conflict, "verifier rejected the commit"});
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::conflict);
    REQUIRE(std::string_view{written.error().message()} == "verifier rejected the commit");
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "original");
  for (const auto& entry : std::filesystem::directory_iterator{workspace}) {
    REQUIRE(entry.path().filename().string().find(".orangutan.tmp.") == std::string::npos);
  }
}

TEST_CASE("FileMutation commit verifier observes the current target fingerprint",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-verifier-ok"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  std::optional<io::FileFingerprint> observed;
  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{.atomic = true};
    options.verify_before_commit = [&observed](const io::FileFingerprint& fp) -> core::Result<void> {
      observed = fp;
      return {};
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE(written.has_value());
  });

  REQUIRE(observed.has_value());
  REQUIRE(observed->size_bytes == 8);
  REQUIRE(observed->mtime_ns > 0);
  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "agent-version");
}

TEST_CASE("FileMutation rejects commit verification outside atomic truncate mode",
          "[unit][io][authority][mutation][atomic]") {
  TempDir temp{"oran-io-mutation-verifier-mode"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  write_direct(workspace / "note.txt", "original");

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    io::WriteTextOptions options{};
    options.verify_before_commit = [](const io::FileFingerprint&) -> core::Result<void> {
      return {};
    };
    auto written =
        co_await io::write_text_file(context.get_executor(), std::move(*mutation), "agent-version", std::move(options));
    REQUIRE_FALSE(written.has_value());
    REQUIRE(written.error().kind() == core::ErrorKind::invalid_argument);
  });

  std::ifstream current{workspace / "note.txt", std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "original");
}

TEST_CASE("FileMutation can replace a writable target without read permission",
          "[unit][io][authority][mutation][permissions]") {
  TempDir temp{"oran-io-mutation-write-only"};
  const auto workspace = temp.path() / "workspace";
  const auto target = workspace / "note.txt";
  std::filesystem::create_directories(workspace);
  write_direct(target, "old");
  std::filesystem::permissions(target, std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation("note.txt");
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "new",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  std::ifstream current{target, std::ios::binary};
  REQUIRE(std::string{std::istreambuf_iterator<char>{current}, std::istreambuf_iterator<char>{}} == "new");
}

TEST_CASE("FileMutation temp naming supports near-limit target names", "[unit][io][authority][mutation][name-max]") {
  TempDir temp{"oran-io-mutation-long-name"};
  const auto workspace = temp.path() / "workspace";
  std::filesystem::create_directories(workspace);
  const auto name = std::string(240U, 'n');

  auto authority = io::DirectoryAuthority::open_trusted(workspace.string());
  REQUIRE(authority.has_value());
  auto mutation = authority->begin_file_mutation(name);
  REQUIRE(mutation.has_value());

  test::run_async([&](asio::io_context& context) -> orangutan::async::Awaitable<void> {
    auto written = co_await io::write_text_file(context.get_executor(),
                                                std::move(*mutation),
                                                "content",
                                                io::WriteTextOptions{.atomic = true});
    REQUIRE(written.has_value());
  });

  REQUIRE(std::filesystem::file_size(workspace / name) == 7U);
}
