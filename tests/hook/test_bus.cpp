// tests/hook/test_bus.cpp — `hook::Bus` subscription and advisory publication.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <catch2/catch_test_macros.hpp>

#include <oran/async.hpp>
#include <oran/core/error.hpp>
#include <oran/core/result.hpp>
#include <oran/hook.hpp>

#include "../test-helpers/run_async.hpp"

namespace async = orangutan::async;
namespace core = orangutan::core;
namespace hook = orangutan::hook;
namespace test = orangutan::tests;

namespace {

using namespace std::chrono_literals;

struct Capture {
  std::string sink;
  hook::Event event;
  /// Identity of the shared snapshot; only valid for comparison.
  const hook::Payload* snapshot;
  hook::Payload payload;
  std::string input_json;
  std::optional<std::string> data_json;
};

[[nodiscard]] hook::Sink recording_sink(std::string id, std::vector<Capture>& captures, bool trusted_local = false) {
  return hook::Sink{
      .id = id,
      .observe = [id, &captures](hook::Event event, hook::PayloadPtr payload) -> async::Awaitable<void> {
        auto capture = Capture{.sink = id, .event = event, .snapshot = payload.get(), .payload = *payload};
        std::visit(
            [&](const auto& alt) {
              if constexpr (requires { alt.input_json; }) {
                capture.input_json = alt.input_json;
              }
            },
            *payload);
        if (const auto* after = std::get_if<hook::ToolAfterPayload>(payload.get()); after != nullptr) {
          capture.data_json = after->data_json;
        }
        captures.push_back(std::move(capture));
        co_return;
      },
      .trusted_local = trusted_local,
  };
}

[[nodiscard]] std::vector<std::string> sink_ids(const std::vector<Capture>& captures) {
  auto ids = std::vector<std::string>{};
  std::ranges::transform(captures, std::back_inserter(ids), &Capture::sink);
  return ids;
}

hook::ToolBeforePayload sample_before() {
  return hook::ToolBeforePayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .started_at = core::Time::epoch(),
  };
}

hook::ToolAfterPayload sample_after_with_data() {
  return hook::ToolAfterPayload{
      .tool_name = "noop",
      .input_json = "{}",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .succeeded = true,
      .output_text = "ok",
      .data_json = std::string{R"({"kind":"sample","raw":true})"},
      .error_kind = "",
      .error_message = "",
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

hook::ToolAfterPayload sample_after_with_redacted_input() {
  return hook::ToolAfterPayload{
      .tool_name = "FileWrite",
      .input_json = R"({"path":"notes.md","content":"secret"})",
      .redacted_input_json = R"({"kind":"redacted_tool_input","input_hash":"abc","content_bytes":6})",
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .succeeded = true,
      .output_text = "ok",
      .error_kind = "",
      .error_message = "",
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

hook::MemoryReadPayload sample_memory_read() {
  return hook::MemoryReadPayload{
      .who = hook::Identity{.scope_key = "scope", .agent_key = "agent", .identity = "operator"},
      .source = "MemoryRecall",
      .query = "sensitive query",
      .redacted_query_bytes = std::string_view{"sensitive query"}.size(),
      .limit = 5,
      .kinds = {"project"},
      .match_count = 1,
      .hits =
          {
              hook::MemoryReadHitPayload{
                  .record =
                      hook::MemoryRecordPayload{
                          .id = "memory-1",
                          .scope_key = "scope",
                          .kind = "project",
                          .title = "Sensitive title",
                          .body = "Sensitive body",
                          .created_at = core::Time::epoch(),
                          .updated_at = core::Time::epoch(),
                          .last_read_at = core::Time::epoch(),
                          .importance = 0.75,
                          .tags = {"secret", "project"},
                          .linked_record_ids = {"linked-1"},
                          .shadow = false,
                      },
                  .score = 0.9,
                  .redacted_record =
                      hook::RedactedMemoryRecordPayload{
                          .id = "memory-1",
                          .scope_key = "scope",
                          .kind = "project",
                          .title_bytes = std::string_view{"Sensitive title"}.size(),
                          .body_bytes = std::string_view{"Sensitive body"}.size(),
                          .tag_count = 2,
                          .linked_record_count = 1,
                          .shadow = false,
                      },
              },
          },
      .started_at = core::Time::epoch(),
      .finished_at = core::Time::epoch(),
  };
}

/// Spawn `publish_advisory`, wait until `started` is set, cancel it and return
/// once the publish has completed.
async::Awaitable<void> publish_then_cancel(asio::io_context& io, hook::Bus& bus, const bool& started) {
  asio::cancellation_signal cancellation;
  async::Channel<std::monostate> completed{io.get_executor(), 1};
  std::exception_ptr failure;
  asio::co_spawn(io,
                 bus.publish_advisory(hook::Event::tool_before, sample_before()),
                 asio::bind_cancellation_slot(cancellation.slot(), [&](std::exception_ptr error) {
                   failure = error;
                   [[maybe_unused]] auto signaled = completed.try_send(std::monostate{});
                 }));
  while (!started) {
    auto yielded = co_await async::sleep_for(io.get_executor(), 1ms);
    REQUIRE(yielded.has_value());
  }
  cancellation.emit(asio::cancellation_type::all);
  auto signaled = co_await completed.receive();
  REQUIRE(signaled.has_value());
  if (failure) {
    std::rethrow_exception(failure);
  }
}

}  // namespace

TEST_CASE("publish_advisory on an empty bus completes", "[hook][bus]") {
  hook::Bus bus;
  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
  });
}

TEST_CASE("subscribe delivers only the subscribed events", "[hook][bus]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(recording_sink("recorder", captures), {hook::Event::tool_before, hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
    co_await bus.publish_advisory(hook::Event::memory_forget, hook::MemoryForgetPayload{});
    co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_data());
  });

  REQUIRE(captures.size() == 2);
  REQUIRE(captures[0].event == hook::Event::tool_before);
  REQUIRE(std::holds_alternative<hook::ToolBeforePayload>(captures[0].payload));
  REQUIRE(captures[1].event == hook::Event::tool_after);
}

TEST_CASE("every observer receives one callback per publish", "[hook][bus]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(recording_sink("first", captures), {hook::Event::tool_before});
  bus.subscribe(hook::Sink{.id = "gate-only"}, {hook::Event::tool_before});
  bus.subscribe(recording_sink("second", captures), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
  });

  auto ids = sink_ids(captures);
  std::ranges::sort(ids);
  REQUIRE(ids == std::vector<std::string>{"first", "second"});
}

TEST_CASE("publish_advisory runs observers concurrently", "[hook][bus]") {
  hook::Bus bus;
  std::size_t active = 0;
  std::size_t peak_active = 0;
  std::vector<std::string> completions;
  const auto delayed = [&](std::string id, std::chrono::milliseconds delay) {
    return hook::Sink{
        .id = id,
        .observe = [&, id, delay](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
          ++active;
          peak_active = std::max(peak_active, active);
          auto slept = co_await async::sleep_for(co_await asio::this_coro::executor, delay);
          --active;
          if (slept) {
            completions.push_back(id);
          }
        },
    };
  };
  bus.subscribe(delayed("first", 40ms), {hook::Event::tool_before});
  bus.subscribe(delayed("second", 10ms), {hook::Event::tool_before});
  bus.subscribe(delayed("third", 20ms), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
  });

  REQUIRE(peak_active == 3);
  REQUIRE(completions == std::vector<std::string>{"second", "third", "first"});
}

TEST_CASE("publish_advisory joins cancelled observers before returning", "[hook][bus][cancellation]") {
  hook::Bus bus;
  bool active = false;
  bool finished = false;
  bool cancellation_seen = false;
  bus.subscribe(
      hook::Sink{
          .id = "cancellation-aware",
          .observe = [&](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
            active = true;
            auto slept = co_await async::sleep_for(co_await asio::this_coro::executor, 5s);
            active = false;
            finished = true;
            cancellation_seen = !slept && slept.error().kind() == core::ErrorKind::cancelled;
          },
      },
      {hook::Event::tool_before});

  test::run_async([&](asio::io_context& io) { return publish_then_cancel(io, bus, active); }, 1s);

  REQUIRE_FALSE(active);
  REQUIRE(finished);
  REQUIRE(cancellation_seen);
}

TEST_CASE("publish_advisory abandons an observer that ignores cancellation at the deadline",
          "[hook][bus][cancellation]") {
  auto bus = std::make_unique<hook::Bus>(hook::BusOptions{.advisory_timeout = 100ms});
  auto active = std::make_shared<bool>(false);
  auto finished = std::make_shared<bool>(false);
  bus->subscribe(
      hook::Sink{
          .id = "cancellation-ignoring",
          .observe = [active, finished](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
            *active = true;
            co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
            static_cast<void>(co_await async::sleep_for(co_await asio::this_coro::executor, 300ms));
            *finished = true;
          },
      },
      {hook::Event::tool_before});

  test::run_async(
      [&](asio::io_context& io) -> async::Awaitable<void> {
        co_await publish_then_cancel(io, *bus, *active);
        // The publish resumed at the 100 ms deadline while the sink still runs.
        REQUIRE_FALSE(*finished);
        // The bus is not poisoned: a later publish is bounded the same way.
        co_await bus->publish_advisory(hook::Event::tool_before, sample_before());
        REQUIRE_FALSE(*finished);
        // The abandoned observer shares ownership of its sink, so it may
        // outlive the bus.
        bus.reset();
        while (!*finished) {
          auto yielded = co_await async::sleep_for(io.get_executor(), 5ms);
          REQUIRE(yielded.has_value());
        }
      },
      2s);
}

TEST_CASE("observer failures do not reach the publisher or siblings", "[hook][bus]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(hook::Sink{.id = "throws",
                           .observe = [](hook::Event, hook::PayloadPtr) -> async::Awaitable<void> {
                             throw std::runtime_error{"boom"};
                             co_return;
                           }},
                {hook::Event::tool_before});
  bus.subscribe(recording_sink("kept", captures), {hook::Event::tool_before});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_before, sample_before());
  });

  REQUIRE(sink_ids(captures) == std::vector<std::string>{"kept"});
}

TEST_CASE("publish_advisory redacts tool output and input for untrusted sinks", "[hook][bus][redaction]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(recording_sink("default", captures), {hook::Event::tool_after});
  bus.subscribe(recording_sink("trusted", captures, true), {hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_data());
    co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_redacted_input());
  });

  const auto of = [&](std::string_view sink, std::size_t publish) {
    auto matches = captures | std::views::filter([&](const Capture& c) { return c.sink == sink; });
    return *std::ranges::next(matches.begin(), static_cast<std::ptrdiff_t>(publish));
  };
  REQUIRE_FALSE(of("default", 0).data_json.has_value());
  REQUIRE(of("trusted", 0).data_json == R"({"kind":"sample","raw":true})");
  REQUIRE(of("default", 1).input_json == R"({"kind":"redacted_tool_input","input_hash":"abc","content_bytes":6})");
  REQUIRE(of("trusted", 1).input_json == R"({"path":"notes.md","content":"secret"})");
}

TEST_CASE("publish_advisory redacts memory read query and records for untrusted sinks",
          "[hook][bus][redaction][memory]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(recording_sink("default", captures), {hook::Event::memory_read_after});
  bus.subscribe(recording_sink("trusted", captures, true), {hook::Event::memory_read_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::memory_read_after, sample_memory_read());
  });

  REQUIRE(captures.size() == 2);
  const auto& redacted =
      std::get<hook::MemoryReadPayload>(std::ranges::find(captures, std::string{"default"}, &Capture::sink)->payload);
  const auto& original =
      std::get<hook::MemoryReadPayload>(std::ranges::find(captures, std::string{"trusted"}, &Capture::sink)->payload);

  REQUIRE(redacted.source == "MemoryRecall");
  REQUIRE(redacted.query.empty());
  REQUIRE(redacted.redacted_query_bytes == std::string_view{"sensitive query"}.size());
  REQUIRE(redacted.hits.size() == 1);
  REQUIRE(redacted.hits[0].record.id == "memory-1");
  REQUIRE(redacted.hits[0].record.title.empty());
  REQUIRE(redacted.hits[0].record.body.empty());
  REQUIRE(redacted.hits[0].record.tags.empty());
  REQUIRE(redacted.hits[0].record.linked_record_ids.empty());
  REQUIRE(redacted.hits[0].redacted_record->body_bytes == std::string_view{"Sensitive body"}.size());

  REQUIRE(original.query == "sensitive query");
  REQUIRE(original.hits[0].record.title == "Sensitive title");
  REQUIRE(original.hits[0].record.body == "Sensitive body");
  REQUIRE(original.hits[0].record.tags == std::vector<std::string>{"secret", "project"});
  REQUIRE(original.hits[0].record.linked_record_ids == std::vector<std::string>{"linked-1"});
}

TEST_CASE("publish_advisory shares one snapshot per trust level", "[hook][bus][redaction]") {
  hook::Bus bus;
  std::vector<Capture> captures;
  bus.subscribe(recording_sink("default-1", captures), {hook::Event::tool_after});
  bus.subscribe(recording_sink("default-2", captures), {hook::Event::tool_after});
  bus.subscribe(recording_sink("trusted", captures, true), {hook::Event::tool_after});

  test::run_async([&](asio::io_context& /*io*/) -> async::Awaitable<void> {
    co_await bus.publish_advisory(hook::Event::tool_after, sample_after_with_redacted_input());
  });

  REQUIRE(captures.size() == 3);
  const auto snapshot = [&](std::string_view sink) {
    return std::ranges::find(captures, sink, &Capture::sink)->snapshot;
  };
  const auto* default_1 = snapshot("default-1");
  const auto* default_2 = snapshot("default-2");
  const auto* trusted = snapshot("trusted");
  REQUIRE(default_1 == default_2);
  REQUIRE(default_1 != trusted);
}
