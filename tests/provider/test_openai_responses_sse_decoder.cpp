// tests/provider/test_openai_responses_sse_decoder.cpp - OpenAI Responses SSE decoder coverage.
//
// The decoder lives under src/oran-provider/_impl/ so we reach it through a
// relative path; it is an internal compilation detail, not part of the public
// surface. Definitions are linked from the oran-provider static library.

#include "../../src/oran-provider/_impl/openai_responses_sse_decoder.hpp"

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
using Decoder = provider::detail::OpenAiResponsesSseDecoder;

using Event = std::pair<std::string, std::string>;

provider::ModelTarget target() {
  return provider::ModelTarget{
      .profile = "openai-main",
      .model = "gpt-test",
      .protocol = provider::ProtocolKind::openai_responses,
      .thinking_budget = std::nullopt,
      .cache = std::nullopt,
  };
}

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

constexpr std::string_view kTextResponse = R"json({
  "id": "resp_1",
  "object": "response",
  "status": "completed",
  "model": "gpt-test",
  "output": [
    {
      "id": "msg_1",
      "type": "message",
      "status": "completed",
      "role": "assistant",
      "content": [{"type": "output_text", "text": "Hello world"}]
    }
  ],
  "usage": {
    "input_tokens": 10,
    "input_tokens_details": {"cached_tokens": 3},
    "output_tokens": 4,
    "total_tokens": 14
  }
})json";

constexpr std::string_view kToolResponse = R"json({
  "id": "resp_2",
  "object": "response",
  "status": "completed",
  "model": "gpt-test",
  "output": [
    {
      "id": "fc_1",
      "type": "function_call",
      "call_id": "call_1",
      "name": "FileRead",
      "arguments": "{\"path\":\"README.md\"}",
      "status": "completed"
    }
  ],
  "usage": {"input_tokens": 5, "output_tokens": 2}
})json";

}  // namespace

TEST_CASE("openai responses sse decoder assembles a text turn from the terminal response", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.created",
           R"({"type":"response.created","response":{"id":"resp_1","status":"in_progress","output":[]},"sequence_number":1})"},
          {"response.output_item.added",
           R"({"type":"response.output_item.added","output_index":0,"item":{"id":"msg_1","type":"message","status":"in_progress","role":"assistant","content":[]},"sequence_number":2})"},
          {"response.output_text.delta",
           R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":"Hello","sequence_number":3})"},
          {"response.output_text.delta",
           R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":" world","sequence_number":4})"},
          {"response.completed",
           std::string{R"({"type":"response.completed","response":)"} + std::string{kTextResponse} +
               R"(,"sequence_number":5})"},
      });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  const auto body = provider::decode_protocol_response(kTextResponse, target());
  REQUIRE(body.has_value());

  REQUIRE(*streamed == *body);
  REQUIRE(streamed->stop_reason == core::StopReason::end_turn);
  REQUIRE(streamed->usage.input_tokens == 10);
  REQUIRE(streamed->usage.cache_read_tokens == 3);
  REQUIRE(sink.log == std::vector<std::string>{"text:Hello", "text: world", "done"});
  REQUIRE(sink.done == core::StopReason::end_turn);
}

TEST_CASE("openai responses sse decoder streams reasoning and function-call arguments", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.reasoning_summary_text.delta",
           R"({"type":"response.reasoning_summary_text.delta","item_id":"rs_1","output_index":0,"summary_index":0,"delta":"checking","sequence_number":1})"},
          {"response.output_item.added",
           R"({"type":"response.output_item.added","output_index":0,"item":{"id":"fc_1","type":"function_call","status":"in_progress","call_id":"call_1","name":"FileRead","arguments":""},"sequence_number":2})"},
          {"response.function_call_arguments.delta",
           R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","output_index":0,"delta":"{\"path\":","sequence_number":3})"},
          {"response.function_call_arguments.delta",
           R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","output_index":0,"delta":"\"README.md\"}","sequence_number":4})"},
          {"response.function_call_arguments.done",
           R"({"type":"response.function_call_arguments.done","item_id":"fc_1","name":"FileRead","output_index":0,"arguments":"{\"path\":\"README.md\"}","sequence_number":5})"},
          {"response.completed",
           std::string{R"({"type":"response.completed","response":)"} + std::string{kToolResponse} +
               R"(,"sequence_number":6})"},
      });

  const auto streamed = decoder.result();
  REQUIRE(streamed.has_value());

  const auto body = provider::decode_protocol_response(kToolResponse, target());
  REQUIRE(body.has_value());

  REQUIRE(*streamed == *body);
  REQUIRE(streamed->stop_reason == core::StopReason::tool_use);
  const auto& tool = std::get<core::ToolUseContent>(streamed->blocks.front());
  REQUIRE(tool.id == "call_1");
  REQUIRE(tool.name == "FileRead");
  REQUIRE(tool.input_json == R"({"path":"README.md"})");
  REQUIRE(sink.log == std::vector<std::string>{
                          "thinking:checking",
                          "tool_start:call_1:FileRead",
                          R"(tool_delta:call_1:{"path":)",
                          R"(tool_delta:call_1:"README.md"})",
                          "done",
                      });
  REQUIRE(sink.done == core::StopReason::tool_use);
}

TEST_CASE("openai responses sse decoder maps incomplete terminal responses through the body decoder",
          "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.output_text.delta",
           R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":"partial","sequence_number":1})"},
          {"response.incomplete",
           R"({"type":"response.incomplete","response":{"id":"resp_3","object":"response","status":"incomplete","model":"gpt-test","output":[],"incomplete_details":{"reason":"max_output_tokens"},"usage":{"input_tokens":1,"output_tokens":1}},"sequence_number":2})"},
      });

  const auto streamed = decoder.result();

  REQUIRE(streamed.has_value());
  REQUIRE(streamed->stop_reason == core::StopReason::max_tokens);
  REQUIRE(sink.log == std::vector<std::string>{"text:partial", "done"});
  REQUIRE(sink.done == core::StopReason::max_tokens);
}

TEST_CASE("openai responses sse decoder surfaces an error event as upstream", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.created",
           R"({"type":"response.created","response":{"id":"resp_1","status":"in_progress","output":[]},"sequence_number":1})"},
          {"error",
           R"({"type":"error","code":"server_error","message":"Something went wrong","param":null,"sequence_number":2})"},
          {"response.completed",
           std::string{R"({"type":"response.completed","response":)"} + std::string{kTextResponse} +
               R"(,"sequence_number":3})"},
      });

  const auto streamed = decoder.result();

  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::upstream);
  REQUIRE(sink.done == std::nullopt);
}

TEST_CASE("openai responses sse decoder surfaces response.failed as a retryable provider error",
          "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.failed",
           R"({"type":"response.failed","response":{"id":"resp_failed","object":"response","status":"failed","model":"gpt-test","output":[],"error":{"code":"server_error","message":"The provider failed while processing the request."},"usage":null},"sequence_number":1})"},
      });

  const auto streamed = decoder.result();

  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::upstream);
  REQUIRE(streamed.error().retryable());
  REQUIRE(sink.done == std::nullopt);
}

TEST_CASE("openai responses sse decoder rejects a stream that never completes", "[unit][provider][sse]") {
  RecordingSink sink;
  Decoder decoder{target(), &sink};
  feed(
      decoder,
      {
          {"response.output_text.delta",
           R"({"type":"response.output_text.delta","item_id":"msg_1","output_index":0,"content_index":0,"delta":"partial","sequence_number":1})"},
      });

  const auto streamed = decoder.result();

  REQUIRE_FALSE(streamed.has_value());
  REQUIRE(streamed.error().kind() == core::ErrorKind::parsing);
  REQUIRE(sink.done == std::nullopt);
}
