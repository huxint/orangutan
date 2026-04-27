#include "providers/protocols/anthropic-messages.hpp"
#include "providers/protocols/openai-chat-completions.hpp"
#include "providers/protocols/openai-responses.hpp"
#include "providers/protocols/protocol-json.hpp"
#include "providers/protocols/provider-registry.hpp"
#include "test-provider-support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {

    void check_provider_parse_error(const orangutan::ProviderError &error, std::string_view expected_label) {
        CHECK(error.category() == orangutan::error_category::parsing);
        CHECK(std::string_view(error.what()).contains(expected_label) == true);
    }

    template <typename Action>
    void expect_provider_parse_error(Action action, std::string_view expected_label) {
        bool caught = false;
        try {
            action();
        } catch (const orangutan::ProviderError &error) {
            caught = true;
            check_provider_parse_error(error, expected_label);
        }

        CHECK(caught == true);
    }

    TEST_CASE("protocol_json_parser_rejects_malformed_or_non_object_payloads") {
        expect_provider_parse_error(
            [] {
                static_cast<void>(orangutan::providers::protocols::parse_protocol_json_object("{bad-json", "test protocol", "response"));
            },
            "test protocol response:");

        expect_provider_parse_error(
            [] {
                static_cast<void>(orangutan::providers::protocols::parse_protocol_json_object("[]", "test protocol", "response"));
            },
            "test protocol response: expected a json object");
    }

    TEST_CASE("protocol_adapters_report_labeled_parse_errors_for_malformed_payloads") {
        const auto chat_adapter = orangutan::providers::protocols::make_openai_chat_completions_adapter();
        expect_provider_parse_error(
            [&chat_adapter] {
                static_cast<void>(chat_adapter->parse_response(orangutan::providers::transport::HttpResponse{.status_code = 200, .body = "{bad-json"}));
            },
            "openai chat completions response:");
        auto chat_decoder = chat_adapter->make_stream_decoder({});
        expect_provider_parse_error(
            [&chat_decoder] {
                chat_decoder->on_event("", "{bad-json");
            },
            "openai chat completions stream event:");

        const auto responses_adapter = orangutan::providers::protocols::make_openai_responses_adapter();
        expect_provider_parse_error(
            [&responses_adapter] {
                static_cast<void>(responses_adapter->parse_response(orangutan::providers::transport::HttpResponse{.status_code = 200, .body = "{bad-json"}));
            },
            "openai responses response:");
        auto responses_decoder = responses_adapter->make_stream_decoder({});
        expect_provider_parse_error(
            [&responses_decoder] {
                responses_decoder->on_event("response.output_text.delta", "{bad-json");
            },
            "openai responses stream event:");

        const auto anthropic_adapter = orangutan::providers::protocols::make_anthropic_messages_adapter();
        expect_provider_parse_error(
            [&anthropic_adapter] {
                static_cast<void>(anthropic_adapter->parse_response(orangutan::providers::transport::HttpResponse{.status_code = 200, .body = "{bad-json"}));
            },
            "anthropic messages response:");
        auto anthropic_decoder = anthropic_adapter->make_stream_decoder({});
        expect_provider_parse_error(
            [&anthropic_decoder] {
                anthropic_decoder->on_event("", "{bad-json");
            },
            "anthropic messages stream event:");
    }

    TEST_CASE("provider_registry_resolves_auth_and_rejects_invalid_combinations") {
        orangutan::providers::protocols::ProviderRegistry registry;

        const auto openai = registry.resolve(orangutan::provider_kind::openai, orangutan::protocol_kind::responses);
        orangutan::providers::transport::header_map openai_headers;
        openai.auth(orangutan::testing::make_test_target("gpt-5"), openai_headers);
        CHECK(openai.adapter->label() == "openai");
        CHECK(openai_headers.at("Authorization") == "Bearer test-key");
        CHECK(openai_headers.at("Content-Type") == "application/json");

        const auto anthropic = registry.resolve(orangutan::provider_kind::anthropic, orangutan::protocol_kind::messages);
        orangutan::providers::transport::header_map anthropic_headers;
        anthropic.auth(orangutan::testing::make_test_target("claude", orangutan::provider_kind::anthropic, orangutan::protocol_kind::messages), anthropic_headers);
        CHECK(anthropic.adapter->label() == "anthropic");
        CHECK(anthropic_headers.at("x-api-key") == "test-key");
        CHECK(anthropic_headers.at("anthropic-version") == "2023-06-01");

        CHECK_THROWS_AS(registry.resolve(orangutan::provider_kind::anthropic, orangutan::protocol_kind::responses), orangutan::ProviderError);
    }

    TEST_CASE("openai_chat_completions_adapter_builds_requests_parses_responses_and_streams_tool_calls") {
        const auto adapter = orangutan::providers::protocols::make_openai_chat_completions_adapter();
        auto target = orangutan::testing::make_test_target("gpt-5-chat", orangutan::provider_kind::openai, orangutan::protocol_kind::chat_completions);
        target.thinking = "high";

        orangutan::ProviderRequest request;
        request.system_prompt = "follow system";
        request.messages = {
            orangutan::Message::user().text("hello"),
        };
        request.tools = {
            orangutan::ToolDef{
                .name = "lookup",
                .description = "lookup data",
                .input_schema = nlohmann::json::object(),
            },
        };
        request.options.max_tokens = 1024;
        request.options.stream = true;

        const auto http_request = adapter->build_request(target, request);
        const auto body = nlohmann::json::parse(http_request.body);
        CHECK(http_request.url == "https://example.test/v1/chat/completions");
        CHECK(body["model"] == "gpt-5-chat");
        CHECK(body["reasoning_effort"] == "high");
        CHECK(body["stream"] == true);
        CHECK(body["messages"][0]["role"] == "system");
        CHECK(body["messages"][1]["role"] == "user");
        CHECK(body["tools"][0]["function"]["name"] == "lookup");

        const auto parsed = adapter->parse_response(orangutan::providers::transport::HttpResponse{
            .status_code = 200,
            .body = R"json({
              "choices": [{
                "finish_reason": "tool_calls",
                "message": {
                  "reasoning_content": "reasoning",
                  "content": "done",
                  "tool_calls": [{
                    "id": "tool-1",
                    "type": "function",
                    "function": {
                      "name": "lookup",
                      "arguments": "{\"value\":1}"
                    }
                  }]
                }
              }]
            })json",
        });
        CHECK(parsed.stop_reason == orangutan::response_stop_reason::tool_use);
        REQUIRE(parsed.content.size() == 3UL);
        CHECK(std::get<orangutan::Thinking>(parsed.content[0]).thinking == "reasoning");
        CHECK(std::get<orangutan::Text>(parsed.content[1]).text == "done");
        CHECK(std::get<orangutan::ToolUse>(parsed.content[2]).name == "lookup");

        std::vector<orangutan::ProviderEvent> events;
        auto decoder = adapter->make_stream_decoder([&events](const orangutan::ProviderEvent &event) {
            events.push_back(event);
        });
        decoder->on_event("", R"json({"choices":[{"delta":{"content":"he","tool_calls":[{"index":0,"id":"call-1","function":{"name":"lookup","arguments":"{\"a\":"}}]}}]})json");
        decoder->on_event("", R"json({"choices":[{"delta":{"content":"llo","tool_calls":[{"index":0,"function":{"arguments":"1}"}}],"reasoning_content":"think"}}]})json");
        decoder->on_event("", R"json({"choices":[{"finish_reason":"stop","delta":{}}]})json");

        const auto streamed = decoder->finish();
        CHECK(streamed.stop_reason == orangutan::response_stop_reason::end_turn);
        REQUIRE(events.size() == 4UL);
        CHECK(std::holds_alternative<orangutan::TextDelta>(events[0]));
        CHECK(std::holds_alternative<orangutan::ToolCallStarted>(events[1]));
        CHECK(std::holds_alternative<orangutan::TextDelta>(events[2]));
        CHECK(std::holds_alternative<orangutan::ThinkingDelta>(events[3]));
        REQUIRE(streamed.content.size() == 3UL);
        CHECK(std::get<orangutan::Text>(streamed.content[1]).text == "hello");
        CHECK(std::get<orangutan::ToolUse>(streamed.content[2]).input.at("a") == 1);

        CHECK_THROWS_AS(adapter->parse_response(orangutan::providers::transport::HttpResponse{
                            .status_code = 200,
                            .body = "{bad-json",
                        }),
                        orangutan::ProviderError);
        CHECK_THROWS_AS(decoder->on_event("", "{bad-json"), orangutan::ProviderError);
    }

    TEST_CASE("openai_responses_adapter_handles_reasoning_and_function_calls") {
        const auto adapter = orangutan::providers::protocols::make_openai_responses_adapter();
        auto target = orangutan::testing::make_test_target("gpt-5-responses", orangutan::provider_kind::openai, orangutan::protocol_kind::responses);
        target.thinking = "medium";

        orangutan::ProviderRequest request;
        request.system_prompt = "be helpful";
        request.messages = {
            orangutan::Message::user().text("hello"),
        };
        request.options.max_tokens = 2048;
        request.options.stream = true;

        const auto http_request = adapter->build_request(target, request);
        const auto body = nlohmann::json::parse(http_request.body);
        CHECK(http_request.url == "https://example.test/v1/responses");
        CHECK(body["instructions"] == "be helpful");
        CHECK(body["reasoning"]["effort"] == "medium");
        CHECK(body["input"].size() == 1UL);

        const auto parsed = adapter->parse_response(orangutan::providers::transport::HttpResponse{
            .status_code = 200,
            .body = R"json({
              "status": "completed",
              "output_text": "plain text",
              "output": [
                {
                  "type": "reasoning",
                  "summary": [{"text": "reasoning"}]
                },
                {
                  "type": "function_call",
                  "id": "item-1",
                  "call_id": "call-1",
                  "name": "lookup",
                  "arguments": "{\"value\":2}"
                }
              ]
            })json",
        });
        CHECK(parsed.stop_reason == orangutan::response_stop_reason::end_turn);
        REQUIRE(parsed.content.size() == 3UL);
        CHECK(std::get<orangutan::Text>(parsed.content[0]).text == "plain text");
        CHECK(std::get<orangutan::Thinking>(parsed.content[1]).thinking == "reasoning");
        CHECK(std::get<orangutan::ToolUse>(parsed.content[2]).input.at("value") == 2);

        std::vector<orangutan::ProviderEvent> events;
        auto decoder = adapter->make_stream_decoder([&events](const orangutan::ProviderEvent &event) {
            events.push_back(event);
        });
        decoder->on_event("response.output_item.added", R"json({"item":{"type":"function_call","id":"item-1","call_id":"call-1","name":"lookup"}})json");
        decoder->on_event("response.function_call_arguments.delta", R"json({"item_id":"item-1","delta":"{\"value\":2}"})json");
        decoder->on_event("response.reasoning.delta", R"json({"delta":"think"})json");
        decoder->on_event("response.output_text.delta", R"json({"delta":"done"})json");
        decoder->on_event("response.completed", R"json({"response":{"status":"completed"}})json");

        const auto streamed = decoder->finish();
        REQUIRE(events.size() == 3UL);
        CHECK(std::holds_alternative<orangutan::ToolCallStarted>(events[0]));
        CHECK(std::holds_alternative<orangutan::ThinkingDelta>(events[1]));
        CHECK(std::holds_alternative<orangutan::TextDelta>(events[2]));
        REQUIRE(streamed.content.size() == 3UL);
        CHECK(std::get<orangutan::Thinking>(streamed.content[0]).thinking == "think");
        CHECK(std::get<orangutan::Text>(streamed.content[1]).text == "done");
        CHECK(std::get<orangutan::ToolUse>(streamed.content[2]).name == "lookup");

        CHECK_THROWS_AS(adapter->parse_response(orangutan::providers::transport::HttpResponse{
                            .status_code = 200,
                            .body = "{bad-json",
                        }),
                        orangutan::ProviderError);
        auto invalid_decoder = adapter->make_stream_decoder({});
        CHECK_THROWS_AS(invalid_decoder->on_event("response.output_text.delta", "{bad-json"), orangutan::ProviderError);
    }

    TEST_CASE("openai_responses_adapter_preserves_multiple_tool_results_in_history") {
        const auto adapter = orangutan::providers::protocols::make_openai_responses_adapter();
        const auto target = orangutan::testing::make_test_target("gpt-5-responses", orangutan::provider_kind::openai, orangutan::protocol_kind::responses);

        orangutan::ProviderRequest request;
        request.system_prompt = "be helpful";
        request.messages = {
            orangutan::Message::assistant().tool_use(orangutan::ToolUse{"call-1", "lookup", {{"value", 1}}}).tool_use(orangutan::ToolUse{"call-2", "summarize", {{"value", 2}}}),
            orangutan::Message::user().tool_result(orangutan::ToolResult{"call-1", "first"}).tool_result(orangutan::ToolResult{"call-2", "second"}),
        };

        const auto http_request = adapter->build_request(target, request);
        const auto body = nlohmann::json::parse(http_request.body);

        REQUIRE(body["input"].size() == 4UL);
        CHECK(body["input"][0]["type"] == "function_call");
        CHECK(body["input"][0]["call_id"] == "call-1");
        CHECK(body["input"][1]["type"] == "function_call");
        CHECK(body["input"][1]["call_id"] == "call-2");
        CHECK(body["input"][2]["type"] == "function_call_output");
        CHECK(body["input"][2]["call_id"] == "call-1");
        CHECK(body["input"][2]["output"] == "first");
        CHECK(body["input"][3]["type"] == "function_call_output");
        CHECK(body["input"][3]["call_id"] == "call-2");
        CHECK(body["input"][3]["output"] == "second");
    }

    TEST_CASE("anthropic_messages_adapter_builds_thinking_requests_and_parses_stream_blocks") {
        const auto adapter = orangutan::providers::protocols::make_anthropic_messages_adapter();
        auto target = orangutan::testing::make_test_target("claude-sonnet", orangutan::provider_kind::anthropic, orangutan::protocol_kind::messages);
        target.thinking = "low";

        orangutan::ProviderRequest request;
        request.system_prompt = "be careful";
        request.messages = {
            orangutan::Message::user().text("hello"),
        };
        request.options.max_tokens = 4096;
        request.tools = {
            orangutan::ToolDef{
                .name = "lookup",
                .description = "lookup data",
                .input_schema = nlohmann::json::object(),
            },
        };

        const auto http_request = adapter->build_request(target, request);
        const auto body = nlohmann::json::parse(http_request.body);
        CHECK(http_request.url == "https://example.test/v1/messages");
        CHECK(body["system"] == "be careful");
        CHECK(body["thinking"]["budget_tokens"] == 1024);
        CHECK(body["tools"][0]["name"] == "lookup");

        const auto parsed = adapter->parse_response(orangutan::providers::transport::HttpResponse{
            .status_code = 200,
            .body = R"json({
              "stop_reason": "tool_use",
              "content": [
                {"type":"thinking","thinking":"reasoning"},
                {"type":"text","text":"done"},
                {"type":"tool_use","id":"tool-1","name":"lookup","input":{"value":3}}
              ]
            })json",
        });
        CHECK(parsed.stop_reason == orangutan::response_stop_reason::tool_use);
        REQUIRE(parsed.content.size() == 3UL);
        CHECK(std::get<orangutan::Thinking>(parsed.content[0]).thinking == "reasoning");
        CHECK(std::get<orangutan::Text>(parsed.content[1]).text == "done");
        CHECK(std::get<orangutan::ToolUse>(parsed.content[2]).input.at("value") == 3);

        std::vector<orangutan::ProviderEvent> events;
        auto decoder = adapter->make_stream_decoder([&events](const orangutan::ProviderEvent &event) {
            events.push_back(event);
        });
        decoder->on_event("", R"json({"type":"content_block_start","content_block":{"type":"thinking"}})json");
        decoder->on_event("", R"json({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"reason"}})json");
        decoder->on_event("", R"json({"type":"content_block_start","content_block":{"type":"tool_use","id":"tool-1","name":"lookup"}})json");
        decoder->on_event("", R"json({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"value\":3}"}})json");
        decoder->on_event("", R"json({"type":"message_delta","delta":{"stop_reason":"end_turn"}})json");

        const auto streamed = decoder->finish();
        REQUIRE(events.size() == 2UL);
        CHECK(std::holds_alternative<orangutan::ThinkingDelta>(events[0]));
        CHECK(std::holds_alternative<orangutan::ToolCallStarted>(events[1]));
        REQUIRE(streamed.content.size() == 2UL);
        CHECK(std::get<orangutan::Thinking>(streamed.content[0]).thinking == "reason");
        CHECK(std::get<orangutan::ToolUse>(streamed.content[1]).name == "lookup");

        CHECK_THROWS_AS(adapter->parse_response(orangutan::providers::transport::HttpResponse{
                            .status_code = 200,
                            .body = "{bad-json",
                        }),
                        orangutan::ProviderError);
        auto invalid_decoder = adapter->make_stream_decoder({});
        CHECK_THROWS_AS(invalid_decoder->on_event("", "{bad-json"), orangutan::ProviderError);
    }

    TEST_CASE("protocol_adapters_replace_invalid_utf8_when_building_json_requests") {
        const auto adapter = orangutan::providers::protocols::make_anthropic_messages_adapter();
        auto target = orangutan::testing::make_test_target("claude-sonnet", orangutan::provider_kind::anthropic, orangutan::protocol_kind::messages);

        orangutan::ProviderRequest request;
        request.system_prompt = std::string{"system"} + '\xB8';
        request.messages = {
            orangutan::Message::user().text(std::string{"hello"} + '\xB8'),
        };
        request.options.max_tokens = 128;

        const auto http_request = adapter->build_request(target, request);
        const auto body = nlohmann::json::parse(http_request.body);
        CHECK(body["system"].get<std::string>().contains("\xEF\xBF\xBD"));
        CHECK(body["messages"][0]["content"][0]["text"].get<std::string>().contains("\xEF\xBF\xBD"));
    }

} // namespace
