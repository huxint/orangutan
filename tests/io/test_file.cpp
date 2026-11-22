#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <oran/io.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace io = orangutan::io;
namespace test = orangutan::tests;

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

  [[nodiscard]] std::filesystem::path path() const {
    return path_;
  }

private:
  std::filesystem::path path_;
};

void write_direct(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

std::string numbered_line(std::uint64_t line_number, char marker, std::size_t filler_width = 80U) {
  auto line = std::string{"line-"};
  line += std::to_string(line_number);
  line += '-';
  line.push_back(marker);
  line += ':';
  line.append(filler_width, marker);
  line.push_back('\n');
  return line;
}

std::string large_numbered_file(char marker) {
  std::string contents;
  contents.reserve(4096U * 96U);
  for (std::uint64_t line = 1; line <= 4096U; ++line) {
    contents += numbered_line(line, marker);
  }
  return contents;
}

}  // namespace

TEST_CASE("read_text_file returns file contents", "[unit][io][file]") {
  TempDir temp{"oran-io-read"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "hello\norangutan");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file(context.get_executor(), file.string());

    REQUIRE(result.has_value());
    REQUIRE(*result == "hello\norangutan");
  });
}

TEST_CASE("read_text_file reports missing files", "[unit][io][file]") {
  TempDir temp{"oran-io-missing"};
  const auto file = temp.path() / "missing.txt";

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file(context.get_executor(), file.string());

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::not_found);
  });
}

TEST_CASE("read_text_file enforces max_bytes", "[unit][io][file]") {
  TempDir temp{"oran-io-max"};
  const auto file = temp.path() / "large.txt";
  write_direct(file, "123456789");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::read_text_file(context.get_executor(), file.string(), io::ReadTextOptions{.max_bytes = 4});

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file observes cancellation before blocking work", "[unit][io][file]") {
  TempDir temp{"oran-io-cancel"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "content");

  asio::io_context context;
  asio::cancellation_signal signal;
  std::optional<core::Result<std::string>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<core::Result<std::string>> {
        co_return co_await io::read_text_file(context.get_executor(), file.string());
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<std::string> r) {
        failure = ep;
        result = std::move(r);
        context.stop();
      }));

  asio::post(context, [&] { signal.emit(asio::cancellation_type::terminal); });
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
}

TEST_CASE("run_blocking returns the callable result", "[unit][io][blocking]") {
  test::run_async([](asio::io_context& context) -> async::Awaitable<void> {
    bool invoked = false;

    auto result = co_await io::run_blocking(context.get_executor(), [&invoked](std::stop_token) {
      invoked = true;
      return core::Result<int>{42};
    });

    REQUIRE(result.has_value());
    REQUIRE(*result == 42);
    REQUIRE(invoked);
  });
}

TEST_CASE("run_blocking observes cancellation before invoking callable", "[unit][io][blocking]") {
  asio::io_context context;
  asio::cancellation_signal signal;
  bool invoked = false;
  std::optional<core::Result<int>> result;
  std::exception_ptr failure;

  asio::co_spawn(
      context,
      [&]() -> async::Awaitable<core::Result<int>> {
        co_return co_await io::run_blocking(context.get_executor(), [&invoked](std::stop_token) {
          invoked = true;
          return core::Result<int>{42};
        });
      },
      asio::bind_cancellation_slot(signal.slot(), [&](std::exception_ptr ep, core::Result<int> r) {
        failure = ep;
        result = std::move(r);
        context.stop();
      }));

  asio::post(context, [&] { signal.emit(asio::cancellation_type::terminal); });
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->has_value());
  REQUIRE(result->error().kind() == core::ErrorKind::cancelled);
  REQUIRE_FALSE(invoked);
}

TEST_CASE("read_text_file_ranged returns whole file with fingerprint and line span", "[unit][io][range]") {
  TempDir temp{"oran-io-range-whole"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "alpha\nbeta\ngamma\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(result.has_value());
    REQUIRE(result->text == "alpha\nbeta\ngamma\n");
    REQUIRE(result->start_line == 1);
    REQUIRE(result->end_line == 3);
    REQUIRE(result->returned_bytes == 17);
    REQUIRE_FALSE(result->truncated);
    REQUIRE(result->fingerprint.size_bytes == 17);
    REQUIRE(result->fingerprint.mtime_ns > 0);
  });
}

TEST_CASE("read_text_file_ranged counts a trailing partial line", "[unit][io][range]") {
  TempDir temp{"oran-io-range-partial-tail"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "alpha\nbeta\ngamma");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(result.has_value());
    REQUIRE(result->end_line == 3);
  });
}

TEST_CASE("read_text_file_ranged observes a rewrite with unchanged size and mtime", "[unit][io][range][freshness]") {
  TempDir temp{"oran-io-range-rewrite"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "first");
  const auto original_mtime = std::filesystem::last_write_time(file);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto first = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(first.has_value());
    REQUIRE(first->text == "first");

    write_direct(file, "fresh");
    std::filesystem::last_write_time(file, original_mtime);

    auto second = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    REQUIRE(second.has_value());
    REQUIRE(second->text == "fresh");
    REQUIRE(second->fingerprint == first->fingerprint);
  });
}

TEST_CASE("read_text_file_ranged keeps the opened file across pathname replacement", "[unit][io][range][authority]") {
  const bool trusted_path = GENERATE(false, true);
  TempDir temp{"oran-io-range-pinned"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "alpha\nbeta\n");

  auto authority = io::DirectoryAuthority::open_trusted(temp.path().string());
  REQUIRE(authority);
  auto opened = trusted_path ? io::ReadOnlyFile::open_trusted(file.string())
                             : authority->open_file(io::AnchoredPath{.relative_path = "input.txt"});
  REQUIRE(opened);
  std::filesystem::rename(file, temp.path() / "held.txt");
  write_direct(file, "outside\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options{
        .range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 2, .line_count = 1}},
    };
    const auto result = co_await io::read_text_file_ranged(context.get_executor(), std::move(*opened), options);

    REQUIRE(result);
    CHECK(result->text == "beta\n");
    CHECK(result->start_line == 2);
    CHECK(result->end_line == 2);
    CHECK(result->returned_bytes == 5);
    CHECK(result->fingerprint.size_bytes == 11);
    CHECK_FALSE(result->truncated);
  });
}

TEST_CASE("cancelling a queued read leaves another caller's read intact", "[unit][io][range][cancellation]") {
  TempDir temp{"oran-io-read-callers"};
  const auto file = temp.path() / "input.txt";
  write_direct(file, "alpha\nbeta\n");
  asio::io_context workers;

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    asio::cancellation_signal cancel;
    asio::steady_timer finished{context, std::chrono::steady_clock::time_point::max()};
    std::array<std::optional<core::Result<io::ReadTextResult>>, 2> results;
    std::size_t completed = 0;
    std::exception_ptr failure;
    auto complete = [&](std::size_t index) {
      return [&, index](std::exception_ptr error, core::Result<io::ReadTextResult> result) {
        if (error) {
          failure = error;
        }
        results[index] = std::move(result);
        if (++completed == results.size()) {
          finished.cancel();
        }
      };
    };

    asio::co_spawn(asio::make_strand(context),
                   io::read_text_file_ranged(workers.get_executor(), file.string()),
                   asio::bind_cancellation_slot(cancel.slot(), complete(0)));
    asio::co_spawn(asio::make_strand(context),
                   io::read_text_file_ranged(workers.get_executor(), file.string()),
                   complete(1));

    // Both callers suspend before the worker queue is allowed to run.
    co_await asio::post(context, asio::use_awaitable);
    cancel.emit(asio::cancellation_type::terminal);
    workers.run();
    asio::error_code error;
    co_await finished.async_wait(asio::redirect_error(asio::use_awaitable, error));
    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(results[0]);
    REQUIRE_FALSE(results[0]->has_value());
    CHECK(results[0]->error().kind() == core::ErrorKind::cancelled);
    REQUIRE(results[1]);
    REQUIRE(results[1]->has_value());
    CHECK((*results[1])->text == "alpha\nbeta\n");
    CHECK((*results[1])->returned_bytes == 11);
  });
}

TEST_CASE("read_text_file_ranged truncates oversize whole-file reads", "[unit][io][range]") {
  TempDir temp{"oran-io-range-truncate"};
  const auto file = temp.path() / "big.txt";
  write_direct(file, "0123456789");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    auto result =
        co_await io::read_text_file_ranged(context.get_executor(), file.string(), io::ReadTextOptions{.max_bytes = 4});
    REQUIRE(result.has_value());
    REQUIRE(result->text == "0123");
    REQUIRE(result->returned_bytes == 4);
    REQUIRE(result->truncated);
    REQUIRE(result->fingerprint.size_bytes == 10);
  });
}

TEST_CASE("read_text_file_ranged extracts a line range", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "one\ntwo\nthree\nfour\nfive\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 2, .line_count = 3}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "two\nthree\nfour\n");
    REQUIRE(result->start_line == 2);
    REQUIRE(result->end_line == 4);
    REQUIRE(result->returned_bytes == 15);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged caps a line range by max_bytes", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-cap"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "one\ntwo\nthree\nfour\nfive\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 1, .line_count = 5}};
    options.max_bytes = 4;
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->truncated);
    REQUIRE(result->text.size() <= 4);
  });
}

TEST_CASE("read_text_file_ranged returns an empty span past EOF", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-past-eof"};
  const auto file = temp.path() / "lines.txt";
  write_direct(file, "only one line\n");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 10, .line_count = 5}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text.empty());
    REQUIRE(result->start_line == 10);
    REQUIRE(result->end_line == 9);
    REQUIRE(result->returned_bytes == 0);
  });
}

TEST_CASE("read_text_file_ranged extracts a late line range from a large file", "[unit][io][range][lines]") {
  TempDir temp{"oran-io-range-lines-large"};
  const auto file = temp.path() / "large-lines.txt";
  write_direct(file, large_numbered_file('a'));

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 3000, .line_count = 4}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);

    REQUIRE(result.has_value());
    REQUIRE(result->fingerprint.size_bytes > 256U * 1024U);
    REQUIRE(result->text ==
            numbered_line(3000, 'a') + numbered_line(3001, 'a') + numbered_line(3002, 'a') + numbered_line(3003, 'a'));
    REQUIRE(result->start_line == 3000);
    REQUIRE(result->end_line == 3003);
    REQUIRE(result->returned_bytes == result->text.size());
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged extracts a byte range", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes"};
  const auto file = temp.path() / "blob.txt";
  write_direct(file, "abcdefghij");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 3, .length_bytes = 4}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "defg");
    REQUIRE(result->returned_bytes == 4);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged byte range past EOF returns the available tail", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes-overflow"};
  const auto file = temp.path() / "blob.txt";
  write_direct(file, "abcdef");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 4, .length_bytes = 100}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text == "ef");
    REQUIRE(result->returned_bytes == 2);
    REQUIRE_FALSE(result->truncated);
  });
}

TEST_CASE("read_text_file_ranged trims a byte range that splits a UTF-8 code point", "[unit][io][range][bytes]") {
  TempDir temp{"oran-io-range-bytes-utf8"};
  const auto file = temp.path() / "utf8.txt";
  // The Han character "中" is three bytes in UTF-8 (E4 B8 AD). Following it
  // with "x" places one more byte inside the requested span.
  write_direct(file, "\xE4\xB8\xADx");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    // Ask for the first two bytes of the multi-byte character — the helper
    // should adjust the tail back to the previous code-point boundary.
    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 2}};
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE(result.has_value());
    REQUIRE(result->text.empty());
    REQUIRE(result->returned_bytes == 0);
  });
}

TEST_CASE("read_text_file_ranged rejects an empty range", "[unit][io][range]") {
  TempDir temp{"oran-io-range-empty"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{};  // neither lines nor bytes set
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged rejects a range with both lines and bytes set", "[unit][io][range]") {
  TempDir temp{"oran-io-range-both"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{
        .lines = io::FileRange::LineSpan{.start_line = 1, .line_count = 1},
        .bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 1},
    };
    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged rejects zero range fields", "[unit][io][range]") {
  TempDir temp{"oran-io-range-zero"};
  const auto file = temp.path() / "file.txt";
  write_direct(file, "hi");

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    io::ReadTextOptions options;
    options.range = io::FileRange{.lines = io::FileRange::LineSpan{.start_line = 0, .line_count = 1}};
    auto zero_start = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(zero_start.has_value());
    REQUIRE(zero_start.error().kind() == core::ErrorKind::invalid_argument);

    options.range = io::FileRange{.bytes = io::FileRange::ByteSpan{.offset_bytes = 1, .length_bytes = 0}};
    auto zero_length = co_await io::read_text_file_ranged(context.get_executor(), file.string(), options);
    REQUIRE_FALSE(zero_length.has_value());
    REQUIRE(zero_length.error().kind() == core::ErrorKind::invalid_argument);
  });
}

TEST_CASE("read_text_file_ranged returns conflict on mid-read race for large files", "[unit][io][range][race]") {
  TempDir temp{"oran-io-range-race-large"};
  const auto file = temp.path() / "large.txt";
  // 96 KiB — well above the 64 KiB retry threshold so a mid-read race surfaces
  // immediately as `conflict` rather than triggering a retry.
  std::string seed(96U * 1024U, 'a');
  write_direct(file, seed);

  test::run_async([&](asio::io_context& context) -> async::Awaitable<void> {
    // Race the read against a writer that bumps the file's content+mtime
    // mid-flight. The mid-read fingerprint compare must catch the drift.
    auto bump = std::thread{[&] {
      // A short delay nudges the writer past the helper's pre-fingerprint
      // capture without depending on exact scheduling; the test tolerates
      // either ordering by accepting both `conflict` and `value` outcomes
      // and asserting only that no false `value` slips through when conflict
      // is reported.
      std::this_thread::sleep_for(std::chrono::microseconds{50});
      std::ofstream out{file, std::ios::binary | std::ios::trunc};
      std::string fresh(96U * 1024U, 'b');
      out.write(fresh.data(), static_cast<std::streamsize>(fresh.size()));
    }};

    auto result = co_await io::read_text_file_ranged(context.get_executor(), file.string());
    bump.join();
    if (!result.has_value()) {
      REQUIRE(result.error().kind() == core::ErrorKind::conflict);
    } else {
      // No race happened to fire; the result must reflect a complete read.
      REQUIRE(result->returned_bytes == 96U * 1024U);
    }
  });
}
