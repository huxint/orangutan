#include <array>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>

#include <nanobench.h>

#include <oran/core/enum_names.hpp>
#include <oran/provider/protocol_request.hpp>

namespace orangutan::bench {

void register_cache_requests(ankerl::nanobench::Bench& bench) {
  provider::Request request;
  request.system_prompt = std::string(2304, 's');
  request.messages = {core::Message::user_text("user turn")};
  request.tools = {
      {.name = "Lookup",
       .description = "Look up a record",
       .input_schema_json = R"({"type":"object"})",
       .required_capabilities = {}},
  };
  request.max_tokens = 256;
  const auto& tool = request.tools.front();
  request.cache = provider::PromptCacheHints{
      .prefix_hash = 0x12345678,
      .prefix_bytes = request.system_prompt->size() + tool.name.size() + tool.description.size() +
                      tool.input_schema_json.size(),
  };

  for (const auto protocol :
       std::array{provider::ProtocolKind::anthropic_messages, provider::ProtocolKind::openai_responses}) {
    for (const bool enabled : std::array{true, false}) {
      const provider::ModelTarget target{
          .profile = "main",
          .model = "bench-model",
          .protocol = protocol,
          .thinking_budget = std::nullopt,
          .cache = provider::PromptCacheOptions{.enabled = enabled},
      };
      bench.run(std::format("provider.{}.cache_{}", core::enum_name(protocol), enabled ? "enabled" : "disabled"),
                [&request, &target] {
                  const auto encoded = provider::make_protocol_request(request, target);
                  if (!encoded) {
                    std::abort();
                  }
                  ankerl::nanobench::doNotOptimizeAway(encoded->body_json);
                });
    }
  }
}

}  // namespace orangutan::bench
