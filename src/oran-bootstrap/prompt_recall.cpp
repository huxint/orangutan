#include "memory_tools.hpp"

#include <utility>

#include <asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include <oran/core/enum_names.hpp>
#include <oran/tool/builtins.hpp>

namespace orangutan::bootstrap {

async::Awaitable<core::Result<std::string>>
recall_prompt_memory(tool::Registry& registry, tool::DispatchContext& context, tool::MemoryRecallRequest request) {
  auto input = nlohmann::json{{"limit", request.limit}};
  if (!request.kinds.empty()) {
    input["kinds"] = std::move(request.kinds);
  }
  const auto input_json = input.dump();
  auto recalled = co_await registry.dispatch(tool::kMemoryRecallName, input_json, context);
  if (!recalled) {
    if (recalled.error().kind() == core::ErrorKind::cancelled) {
      co_return std::unexpected(std::move(recalled).error());
    }
    co_return std::format("Memory index unavailable ({}). Do not assume there are no saved notes. "
                          "Continue within the available context and permissions.",
                          core::enum_name(recalled.error().kind()));
  }
  auto data = recalled->data_json ? nlohmann::json::parse(*recalled->data_json, nullptr, false) : nlohmann::json{};
  if (recalled->is_error || recalled->usage.truncated || !data.is_object() ||
      data.value("kind", nlohmann::json{}) != "memory_index") {
    // A hook may rewrite the call to a content read. Only an index belongs in
    // the automatic prefix; explicit reads remain ordinary tool results.
    co_return std::string{"Memory index unavailable: no complete index was returned. Do not assume memory is empty."};
  }
  co_return std::move(recalled->text);
}

}  // namespace orangutan::bootstrap
