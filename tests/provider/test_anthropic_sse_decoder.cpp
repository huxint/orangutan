// tests/provider/test_anthropic_sse_decoder.cpp - Anthropic Messages SSE decoder coverage.
//
// The decoder lives under src/oran-provider/_impl/ so we reach it through a
// relative path; it is an internal compilation detail, not part of the public
// surface. Definitions are linked from the oran-provider static library.

#include "../../src/oran-provider/_impl/anthropic_sse_decoder.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <oran/provider.hpp>

namespace {

namespace core = orangutan::core;
namespace provider = orangutan::provider;
using Decoder = provider::detail::AnthropicSseDecoder;

using Event = std::pair<std::string, std::string>;

provider::ModelTarget target() {
  return provider::ModelTarget{
      .profile = "anthropic-main",
      .model = "claude-test",
      .protocol = provider::ProtocolKind::anthropic_messages,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

// A streaming observer that records the ordered delta callbacks plus the final
// stop reason so tests can assert call order, not just the assembled Response.
class RecordingSink final : public provider::EventSink {
public:
  void on_text_delta(std::string_view delta) override {
    log.push_back("text:" + std::string{delta});
  }
  void on_thinking_delta(std::string_view delta) override {
    log.push_back("thinking:" + std::string{delta});
  }
  void on_tool_start(std::string_view id, std::string_view name) override {
    log.push_back("tool_start:" + std::string{id} + ":" + std::string{name});
  }
  void on_tool_delta(std::string_view id, std::string_view input_delta) override {
    log.push_back("tool_delta:" + std::string{id} + ":" + std::string{input_delta});
  }
  void on_done(core::StopReason stop_reason) override {
    done = stop_reason;
    log.push_back("done");
  }

  std::vector<std::string> log;
  std::optional<core::StopReason> done;
};

void feed(Decoder& decoder, const std::vector<Event>& events) {
  for (const auto& [name, data] : events) {
    decoder.consume(name, data);
  }
}

Event message_start(std::string_view usage_json) {
  return {"message_start",
          R"({"type":"message_start","message":{"id":"msg_1","type":"message","role":"assistant",)"
          R"("model":"claude-test","content":[],"stop_reason":null,"usage":)" +
              std::string{usage_json} + "}}"};
}

Event message_delta(std::string_view stop_reason, std::string_view usage_json) {
  return {"message_delta",
          std::string{R"({"type":"message_delta","delta":{"stop_reason":")"} + std::string{stop_reason} +
              R"(","stop_sequence":null},"usage":)" + std::string{usage_json} + "}"};
}

const Event kMessageStop{"message_stop", R"({"type":"message_stop"})"};

}  // namespace

TEST_CASE("anthropic sse decoder assembles a text turn matching the body decoder", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":10,"output_tokens":1,"cache_creation_input_tokens":3,)"
                         R"("cache_read_input_tokens":2})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":" world"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           message_delta("end_turn", R"({"output_tokens":15})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  const auto body = provider::decode_protocol_response(
      R"({"type":"message","role":"assistant","model":"claude-test",)"
      R"("content":[{"type":"text","text":"Hello world"}],"stop_reason":"end_turn",)"
      R"("usage":{"input_tokens":10,"output_tokens":15,"cache_creation_input_tokens":3,"cache_read_input_tokens":2}})",
      target());
  REQUIRE(body.has_value());

  REQUIRE(*streamed == *body);
  REQUIRE(sink.log == std::vector<std::string>{"text:Hello", "text: world", "done"});
  REQUIRE(sink.done == core::StopReason::end_turn);
}

TEST_CASE("anthropic sse decoder assembles a tool_use turn from input_json deltas", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          message_start(R"({"input_tokens":10,"output_tokens":1})"),
          {"content_block_start",
           R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
          {"content_block_delta",
           R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Let me check."}})"},
          {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
          {"content_block_start",
           R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"get_weather","input":{}}})"},
          {"content_block_delta",
           R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"location\":"}})"},
          {"content_block_delta",
           R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"NYC\"}"}})"},
          {"content_block_stop", R"({"type":"content_block_stop","index":1})"},
          message_delta("tool_use", R"({"output_tokens":20})"),
          kMessageStop,
      });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  const auto body = provider::decode_protocol_response(
      R"({"type":"message","role":"assistant","model":"claude-test","content":[)"
      R"({"type":"text","text":"Let me check."},)"
      R"({"type":"tool_use","id":"toolu_1","name":"get_weather","input":{"location":"NYC"}}],)"
      R"("stop_reason":"tool_use","usage":{"input_tokens":10,"output_tokens":20}})",
      target());
  REQUIRE(body.has_value());

  REQUIRE(*streamed == *body);
  REQUIRE(streamed->stop_reason == core::StopReason::tool_use);
  REQUIRE(sink.log == std::vector<std::string>{
                          "text:Let me check.",
                          "tool_start:toolu_1:get_weather",
                          R"(tool_delta:toolu_1:{"location":)",
                          R"(tool_delta:toolu_1:"NYC"})",
                          "done",
                      });
}

TEST_CASE("anthropic sse decoder assembles thinking with a signature", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"reasoning"}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig123"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           message_delta("end_turn", R"({"output_tokens":5})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  const auto body = provider::decode_protocol_response(
      R"({"type":"message","role":"assistant","model":"claude-test",)"
      R"("content":[{"type":"thinking","thinking":"reasoning","signature":"sig123"}],)"
      R"("stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":5}})",
      target());
  REQUIRE(body.has_value());

  REQUIRE(*streamed == *body);
  REQUIRE(sink.log == std::vector<std::string>{"thinking:reasoning", "done"});
}

TEST_CASE("anthropic sse decoder treats text deltas in thinking blocks as thinking", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"hidden reasoning"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           message_delta("end_turn", R"({"output_tokens":5})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  REQUIRE(streamed->blocks.size() == 1);
  REQUIRE(std::get<core::ThinkingContent>(streamed->blocks.front()).thinking == "hidden reasoning");
  REQUIRE(sink.log == std::vector<std::string>{"thinking:hidden reasoning", "done"});
}

TEST_CASE("anthropic sse decoder ignores ping events", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           {"ping", R"({"type":"ping"})"},
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"ping", R"({"type":"ping"})"},
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ok"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           {"ping", R"({"type":"ping"})"},
           message_delta("end_turn", R"({"output_tokens":2})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());
  REQUIRE(std::get<core::TextContent>(streamed->blocks.front()).text == "ok");
  REQUIRE(sink.log == std::vector<std::string>{"text:ok", "done"});
}

TEST_CASE("anthropic sse decoder maps an unknown stop reason to error", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           message_delta("unrecognized_reason", R"({"output_tokens":2})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());
  REQUIRE(streamed->stop_reason == core::StopReason::error);
}

TEST_CASE("anthropic sse decoder surfaces an error event as upstream", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"error", R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"},
           // A later event must not overwrite the recorded error.
           kMessageStop,
       });

  const auto streamed = decoder.result();
  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::upstream);
  REQUIRE(sink.done == std::nullopt);
}

TEST_CASE("anthropic sse decoder rejects malformed event data", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_delta", "this is not json"},
       });

  const auto streamed = decoder.result();
  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::parsing);
}

TEST_CASE("anthropic sse decoder rejects a stream that never completes", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}})"},
           {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
           // No message_delta / message_stop: the stream was cut off.
       });

  const auto streamed = decoder.result();
  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::parsing);
  REQUIRE(sink.done == std::nullopt);
}

TEST_CASE("anthropic sse decoder rejects message_stop with an unfinished content block", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(decoder,
       {
           message_start(R"({"input_tokens":1,"output_tokens":1})"),
           {"content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"},
           {"content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"partial"}})"},
           message_delta("end_turn", R"({"output_tokens":2})"),
           kMessageStop,
       });

  const auto streamed = decoder.result();

  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::parsing);
  REQUIRE(sink.done == std::nullopt);
}
