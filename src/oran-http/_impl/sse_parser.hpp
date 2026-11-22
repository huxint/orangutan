// src/oran-http/_impl/sse_parser.hpp - incremental Server-Sent Events parser.

#pragma once

#include <string>
#include <string_view>

#include <oran/http/client.hpp>

namespace orangutan::http::detail {

/// Parser bounds. `max_line_bytes` guards a never-terminating line (a chunk
/// stream with no '\n'); `max_event_bytes` guards a single giant event's
/// accumulated `data:`. Exceeding either latches `exceeded()` and the parser
/// stops consuming; the transport aborts the transfer on the next write
/// callback, so no partial event is ever dispatched.
struct SseParserOptions {
  std::size_t max_line_bytes{64 * 1024};
  std::size_t max_event_bytes{16 * 1024 * 1024};

  friend bool operator==(const SseParserOptions&, const SseParserOptions&) = default;
};

/// Incremental parser for the `text/event-stream` wire grammar. Feed byte
/// chunks as they arrive; each complete event (terminated by a blank line) is
/// reported through the caller's `on_event`. The parser holds partial lines and
/// partial events across `feed` calls so chunk boundaries are transparent.
class SseParser {
public:
  explicit SseParser(SseParserOptions options = {}) : options_{options} {}

  template <typename OnEvent>
  void feed(std::string_view chunk, OnEvent&& on_event) {
    if (exceeded_) {
      return;
    }
    for (const char byte : chunk) {
      if (byte != '\n') {
        if (pending_line_.size() < options_.max_line_bytes) {
          pending_line_.push_back(byte);
        } else {
          exceeded_ = true;
          return;
        }
        continue;
      }
      auto line = std::string_view{pending_line_};
      if (line.ends_with('\r')) {
        line.remove_suffix(1);
      }
      if (line.empty()) {
        dispatch(on_event);
      } else {
        consume_field(line);
      }
      pending_line_.clear();
    }
  }

  [[nodiscard]] bool exceeded() const noexcept {
    return exceeded_;
  }

private:
  void consume_field(std::string_view line) {
    if (exceeded_) {
      return;
    }
    if (line.front() == ':') {
      return;  // Comment line.
    }
    auto field = line;
    std::string_view value{};
    if (const auto colon = line.find(':'); colon != std::string_view::npos) {
      field = line.substr(0, colon);
      value = line.substr(colon + 1);
      if (value.starts_with(' ')) {
        value.remove_prefix(1);
      }
    }
    if (field == "event") {
      event_type_.assign(value);
    } else if (field == "data") {
      if (data_.size() + value.size() + 1 > options_.max_event_bytes) {
        exceeded_ = true;
        return;
      }
      data_.append(value);
      data_.push_back('\n');
    }
    // `id`, `retry`, and unknown fields are intentionally ignored.
  }

  template <typename OnEvent>
  void dispatch(OnEvent&& on_event) {
    if (exceeded_) {
      return;
    }
    if (data_.empty()) {
      event_type_.clear();  // An event with no data field is not dispatched.
      return;
    }
    if (data_.back() == '\n') {
      data_.pop_back();
    }
    on_event(SseEvent{.event = event_type_.empty() ? std::string{"message"} : event_type_, .data = data_});
    event_type_.clear();
    data_.clear();
  }

  SseParserOptions options_;
  bool exceeded_{false};
  std::string pending_line_;
  std::string event_type_;
  std::string data_;
};

}  // namespace orangutan::http::detail
