// include/oran/tool/output.hpp — structured tool output envelope.
//
// Tool handlers always return model-facing text, and may optionally attach
// structured JSON bytes, attachment metadata, and usage metrics. `data_json`
// is serialized JSON, not a `nlohmann::json` value: provider adapters own
// protocol-specific parsing/serialization, while this public header stays
// third-party-free and cheap to include.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orangutan::tool {

struct ToolUsage {
  std::optional<std::uintmax_t> bytes_read{};
  std::optional<std::uintmax_t> bytes_written{};
  std::optional<std::uint32_t> files_touched{};
  std::optional<std::uint64_t> match_count{};
  std::optional<double> cost_estimate{};
  std::optional<std::chrono::nanoseconds> wall_time{};
  bool truncated{false};
  bool data_dropped{false};

  [[nodiscard]] bool empty() const noexcept {
    return !bytes_read.has_value() && !bytes_written.has_value() && !files_touched.has_value() &&
           !match_count.has_value() && !cost_estimate.has_value() && !wall_time.has_value() && !truncated &&
           !data_dropped;
  }

  friend bool operator==(const ToolUsage&, const ToolUsage&) = default;
};

struct Attachment {
  std::string file_path;
  std::string mime_type;
  std::optional<std::uintmax_t> byte_size{};
  std::optional<std::string> fingerprint{};

  friend bool operator==(const Attachment&, const Attachment&) = default;
};

/// One tool's response. `text` is required and remains the provider fallback
/// for protocols that do not consume structured data yet.
struct Output {
  std::string text;
  std::optional<std::string> data_json{};
  std::vector<Attachment> attachments{};
  ToolUsage usage{};
  bool is_error{false};

  [[nodiscard]] static Output text_only(std::string text) {
    return Output{.text = std::move(text)};
  }

  [[nodiscard]] static Output error(std::string message, std::optional<std::string> data_json = std::nullopt) {
    return Output{.text = std::move(message), .data_json = std::move(data_json), .is_error = true};
  }

  friend bool operator==(const Output&, const Output&) = default;
};

inline constexpr std::size_t kDefaultMaxOutputTextBytes = 256U * 1024U;
inline constexpr std::size_t kDefaultMaxOutputDataBytes = 1024U * 1024U;

/// Byte caps applied at the dispatch/scheduler boundary. A cap value of 0
/// disables that channel's cap; the defaults match spec 0014.
struct OutputCapOptions {
  std::size_t max_text_bytes{kDefaultMaxOutputTextBytes};
  std::size_t max_data_bytes{kDefaultMaxOutputDataBytes};

  friend bool operator==(const OutputCapOptions&, const OutputCapOptions&) = default;
};

struct OutputCapReport {
  bool text_truncated{false};
  bool data_dropped{false};
  std::size_t text_bytes_before{0};
  std::size_t text_bytes_after{0};
  std::size_t data_bytes_before{0};

  [[nodiscard]] bool empty() const noexcept {
    return !text_truncated && !data_dropped;
  }

  friend bool operator==(const OutputCapReport&, const OutputCapReport&) = default;
};

/// Apply text/data byte caps in-place. Text truncation keeps a valid UTF-8
/// code-point boundary when possible, sets `usage.truncated`, and leaves
/// `data_json` untouched. Data overflow drops only `data_json` and sets
/// `usage.data_dropped`; the text fallback remains available.
[[nodiscard]] OutputCapReport apply_output_caps(Output& output, OutputCapOptions options = {});

}  // namespace orangutan::tool
