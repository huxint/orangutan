// include/oran/io/range.hpp — file-view range request and result.
//
// `FileRange` and `ReadTextResult` are the public types of spec 0011 v1's
// range-aware read path. They live in their own header (alongside
// `fingerprint.hpp`) so callers can pull in just the file-view contract
// without dragging the async-aware `file.hpp` surface in.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <oran/io/fingerprint.hpp>

namespace orangutan::io {

/// Mutually exclusive line- or byte-range request. Exactly one of `lines`
/// and `bytes` must be set. The range-aware read returns
/// `Error::invalid_argument` when both or neither is supplied, or when
/// any of the populated branch's fields is zero (defensive guard — to
/// read from byte offset 0 or line 0, omit the range entirely).
struct FileRange {
  /// 1-based line range. `start_line` and `line_count` must both be >= 1.
  /// A `start_line` past EOF yields an empty `ReadTextResult` with
  /// `end_line == start_line - 1`, not an error.
  struct LineSpan {
    std::uint64_t start_line{0};
    std::uint64_t line_count{0};
  };

  /// 1-based byte offset (`offset_bytes` >= 1) plus a non-zero
  /// `length_bytes`. The range is clamped to EOF at read time; no error
  /// is raised for overruns. UTF-8-mid-boundary tails on the end of the
  /// requested span are adjusted down to the nearest code-point boundary.
  struct ByteSpan {
    std::uintmax_t offset_bytes{0};
    std::uintmax_t length_bytes{0};
  };

  std::optional<LineSpan> lines{};
  std::optional<ByteSpan> bytes{};
};

/// Result of a range-aware text read.
///
/// `start_line` / `end_line` are 1-based inclusive bounds of the lines
/// covered by the returned text. For a whole-file read or a line-range
/// read they reflect the actual span; for a byte-range read they remain
/// at their defaults (`start_line = 1`, `end_line = 0`) because mapping
/// arbitrary byte offsets back to line numbers requires a full prefix
/// scan and is deferred to a follow-up slice that introduces the line
/// offset index (spec 0011 v1.1).
///
/// `returned_bytes` is the byte length of `text` — same as
/// `text.size()`, surfaced as a separate field so callers do not have to
/// re-measure it.
///
/// `truncated` fires when the output cap (`ReadTextOptions::max_bytes`)
/// stopped the read before the requested range was satisfied. Whole-file
/// reads whose body fits inside `max_bytes` leave `truncated` false even
/// when the requested range was unbounded.
struct ReadTextResult {
  std::string text{};
  FileFingerprint fingerprint{};
  std::uint64_t start_line{1};
  std::uint64_t end_line{0};
  std::uintmax_t returned_bytes{0};
  bool truncated{false};
};

}  // namespace orangutan::io
