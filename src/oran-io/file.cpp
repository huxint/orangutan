#include <oran/io/file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

#include <oran/core/error.hpp>
#include <oran/io/blocking.hpp>
#include <oran/io/directory_authority.hpp>
#include <oran/io/fingerprint.hpp>

namespace orangutan::io {
namespace {

constexpr std::uintmax_t kMidReadRetryThresholdBytes = 64U * 1024U;

[[nodiscard]] core::Error descriptor_error(std::string message, const ReadOnlyFile& file, int error_number) {
  return core::Error::io(std::move(message))
      .with("path", std::string{file.display_path()})
      .with("errno", std::to_string(error_number))
      .with("detail", std::generic_category().message(error_number));
}

[[nodiscard]] core::Result<std::size_t>
read_chunk(const ReadOnlyFile& file, std::uintmax_t offset, std::span<char> destination) {
  if (offset > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max())) {
    return std::unexpected(
        core::Error::invalid_argument("file read offset is too large").with("path", std::string{file.display_path()}));
  }

  while (true) {
    errno = 0;
    const auto count =
        ::pread(file.native_handle(), destination.data(), destination.size(), static_cast<off_t>(offset));
    if (count >= 0) {
      return static_cast<std::size_t>(count);
    }
    if (errno != EINTR) {
      return std::unexpected(descriptor_error("failed while reading file", file, errno));
    }
  }
}

constexpr std::size_t kChunkSize = 8192U;

[[nodiscard]] std::pair<std::size_t, std::size_t> align_to_utf8_boundaries(std::string_view buffer) noexcept {
  std::size_t head = 0;
  while (head < buffer.size()) {
    const auto byte = static_cast<std::uint8_t>(buffer[head]);
    if ((byte & 0xC0U) != 0x80U) {
      break;
    }
    ++head;
  }

  std::size_t end = head;
  std::size_t cursor = head;
  while (cursor < buffer.size()) {
    const auto byte = static_cast<std::uint8_t>(buffer[cursor]);
    std::size_t length = 0;
    if (byte < 0x80U) {
      length = 1U;
    } else if (byte < 0xC2U) {
      break;
    } else if (byte < 0xE0U) {
      length = 2U;
    } else if (byte < 0xF0U) {
      length = 3U;
    } else if (byte < 0xF5U) {
      length = 4U;
    } else {
      break;
    }
    if (cursor + length > buffer.size()) {
      break;
    }
    cursor += length;
    end = cursor;
  }
  return {head, end};
}

[[nodiscard]] core::Result<void> validate_range(const FileRange& range) {
  const bool has_lines = range.lines.has_value();
  const bool has_bytes = range.bytes.has_value();
  if (has_lines == has_bytes) {
    return std::unexpected(core::Error::invalid_argument("FileRange must specify exactly one of lines or bytes"));
  }
  if (has_lines) {
    if (range.lines->start_line == 0 || range.lines->line_count == 0) {
      return std::unexpected(core::Error::invalid_argument("line range fields must be non-zero"));
    }
    return {};
  }
  if (range.bytes->offset_bytes == 0 || range.bytes->length_bytes == 0) {
    return std::unexpected(core::Error::invalid_argument("byte range fields must be non-zero"));
  }
  return {};
}

[[nodiscard]] std::uint64_t count_line_span(std::string_view text) noexcept {
  if (text.empty()) {
    return 0;
  }
  const auto newlines = static_cast<std::uint64_t>(std::ranges::count(text, '\n'));
  return newlines + (text.back() == '\n' ? 0U : 1U);
}

[[nodiscard]] core::Result<std::string>
read_bytes(const ReadOnlyFile& file, std::uintmax_t offset, std::uintmax_t length) {
  std::string text;
  // Tool input can control `length`; cap eager allocation and let actual
  // bytes read drive any later growth.
  text.reserve(static_cast<std::size_t>(std::min<std::uintmax_t>(length, 64U * 1024U)));
  std::array<char, kChunkSize> buffer{};
  while (static_cast<std::uintmax_t>(text.size()) < length) {
    const auto remaining = length - static_cast<std::uintmax_t>(text.size());
    const auto requested = static_cast<std::size_t>(std::min<std::uintmax_t>(buffer.size(), remaining));
    auto count =
        read_chunk(file, offset + static_cast<std::uintmax_t>(text.size()), std::span<char>{buffer.data(), requested});
    if (!count) {
      return std::unexpected(std::move(count).error());
    }
    if (*count == 0U) {
      break;
    }
    text.append(buffer.data(), *count);
  }
  return text;
}

[[nodiscard]] core::Result<ReadTextResult>
read_whole(const ReadOnlyFile& file, std::uintmax_t file_size, std::uintmax_t max_bytes) {
  const auto requested = std::min(file_size, max_bytes);
  auto text = read_bytes(file, 0U, requested);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  ReadTextResult result;
  result.text = std::move(*text);
  result.truncated = file_size > max_bytes;
  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  result.start_line = 1;
  result.end_line = count_line_span(result.text);
  return result;
}

[[nodiscard]] core::Result<ReadTextResult>
read_lines(const ReadOnlyFile& file, FileRange::LineSpan lines, std::uintmax_t max_bytes) {
  ReadTextResult result;
  result.start_line = lines.start_line;
  result.end_line = lines.start_line - 1U;

  std::uint64_t current_line = 1;
  std::uint64_t emitted_lines = 0;
  std::uintmax_t offset = 0;
  std::array<char, kChunkSize> buffer{};
  while (emitted_lines < lines.line_count) {
    auto count = read_chunk(file, offset, buffer);
    if (!count) {
      return std::unexpected(std::move(count).error());
    }
    if (*count == 0U) {
      break;
    }
    offset += *count;

    for (std::size_t index = 0; index < *count; ++index) {
      const char ch = buffer[index];
      const bool in_range = current_line >= lines.start_line && emitted_lines < lines.line_count;
      if (in_range) {
        if (static_cast<std::uintmax_t>(result.text.size()) >= max_bytes) {
          result.truncated = true;
          break;
        }
        result.text.push_back(ch);
      }
      if (ch == '\n') {
        if (in_range) {
          ++emitted_lines;
          result.end_line = lines.start_line + emitted_lines - 1U;
        }
        ++current_line;
      }
    }
    if (result.truncated) {
      break;
    }
  }

  if (!result.text.empty() && result.text.back() != '\n' && emitted_lines < lines.line_count) {
    ++emitted_lines;
    result.end_line = lines.start_line + emitted_lines - 1U;
  }
  if (result.truncated && !result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

[[nodiscard]] core::Result<ReadTextResult>
read_bytes_range(const ReadOnlyFile& file, FileRange::ByteSpan bytes, std::uintmax_t max_bytes) {
  const auto requested = std::min(bytes.length_bytes, max_bytes);
  auto text = read_bytes(file, bytes.offset_bytes, requested);
  if (!text) {
    return std::unexpected(std::move(text).error());
  }

  ReadTextResult result;
  result.text = std::move(*text);
  result.truncated = max_bytes < bytes.length_bytes && result.text.size() >= max_bytes;
  if (!result.text.empty()) {
    const auto [head, end] = align_to_utf8_boundaries(result.text);
    result.text = result.text.substr(head, end - head);
  }
  result.returned_bytes = static_cast<std::uintmax_t>(result.text.size());
  return result;
}

[[nodiscard]] core::Result<ReadTextResult>
dispatch_read(const ReadOnlyFile& file, const ReadTextOptions& options, const FileFingerprint& fingerprint) {
  if (!options.range) {
    return read_whole(file, fingerprint.size_bytes, options.max_bytes);
  }
  if (options.range->lines) {
    return read_lines(file, *options.range->lines, options.max_bytes);
  }
  return read_bytes_range(file, *options.range->bytes, options.max_bytes);
}

[[nodiscard]] core::Result<ReadTextResult> read_file(const ReadOnlyFile& file, const ReadTextOptions& options) {
  if (options.max_bytes == 0U) {
    return std::unexpected(core::Error::invalid_argument("max_bytes must be greater than zero")
                               .with("path", std::string{file.display_path()}));
  }
  if (options.range) {
    if (auto valid = validate_range(*options.range); !valid) {
      return std::unexpected(std::move(valid).error().with("path", std::string{file.display_path()}));
    }
  }

  auto pre = compute_file_fingerprint(file);
  if (!pre) {
    return std::unexpected(std::move(pre).error());
  }

  auto result = dispatch_read(file, options, *pre);
  if (!result) {
    return std::unexpected(std::move(result).error());
  }
  auto post = compute_file_fingerprint(file);
  if (!post) {
    return std::unexpected(std::move(post).error());
  }

  if (*pre != *post) {
    const bool ranged = options.range.has_value();
    const bool large = pre->size_bytes >= kMidReadRetryThresholdBytes;
    if (ranged || large) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during read"}
                                 .with("path", std::string{file.display_path()})
                                 .with("size_before", std::to_string(pre->size_bytes))
                                 .with("size_after", std::to_string(post->size_bytes)));
    }

    pre = post;
    result = dispatch_read(file, options, *pre);
    if (!result) {
      return std::unexpected(std::move(result).error());
    }
    post = compute_file_fingerprint(file);
    if (!post) {
      return std::unexpected(std::move(post).error());
    }
    if (*pre != *post) {
      return std::unexpected(core::Error{core::ErrorKind::conflict, "file changed during read (after retry)"}.with(
          "path",
          std::string{file.display_path()}));
    }
  }

  result->fingerprint = *post;
  return result;
}

}  // namespace

async::Awaitable<core::Result<std::string>>
read_text_file(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  const auto original_path = path;
  auto result = co_await read_text_file_ranged(std::move(executor), std::move(path), options);
  if (!result) {
    co_return std::unexpected(std::move(result).error());
  }
  if (result->truncated) {
    co_return std::unexpected(core::Error::invalid_argument("file exceeds max_bytes")
                                  .with("path", original_path)
                                  .with("max_bytes", std::to_string(options.max_bytes)));
  }
  co_return std::move(result->text);
}

async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, std::string path, ReadTextOptions options) {
  co_return co_await run_blocking(std::move(executor),
                                  [path = std::move(path), options](std::stop_token) -> core::Result<ReadTextResult> {
                                    auto file = ReadOnlyFile::open_trusted(path);
                                    if (!file) {
                                      return std::unexpected(std::move(file).error());
                                    }
                                    return read_file(*file, options);
                                  });
}

async::Awaitable<core::Result<ReadTextResult>>
read_text_file_ranged(asio::any_io_executor executor, ReadOnlyFile file, ReadTextOptions options) {
  co_return co_await run_blocking(std::move(executor), [file = std::move(file), options](std::stop_token) {
    return read_file(file, options);
  });
}

async::Awaitable<core::Result<void>>
write_text_file(asio::any_io_executor executor, FileMutation mutation, std::string contents, WriteTextOptions options) {
  co_return co_await run_blocking(
      std::move(executor),
      [mutation = std::move(mutation), contents = std::move(contents), options = std::move(options)](
          std::stop_token) mutable {
        try {
          return mutation.write_text(contents, std::move(options));
        } catch (const std::exception& error) {
          return core::Result<void>{
              std::unexpected(core::Error::io("anchored write failed").with("detail", error.what()))};
        } catch (...) {
          return core::Result<void>{std::unexpected(core::Error::io("anchored write failed"))};
        }
      });
}

}  // namespace orangutan::io
