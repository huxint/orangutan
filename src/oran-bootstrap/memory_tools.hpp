#pragma once

#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>

namespace orangutan::memory::longterm {
class Backend;
}  // namespace orangutan::memory::longterm

namespace orangutan::tool {
class Registry;
struct DispatchContext;
struct MemoryRecallRequest;
}  // namespace orangutan::tool

namespace orangutan::bootstrap {

// Services outlive the dispatch context and every tool borrowing it. Scope is
// captured from the host; tool inputs cannot select a different memory owner.
void bind_memory_tools(tool::DispatchContext& context,
                       memory::longterm::Backend& backend,
                       std::string scope_key);

[[nodiscard]] async::Awaitable<core::Result<std::string>>
recall_prompt_memory(tool::Registry& registry, tool::DispatchContext& context, tool::MemoryRecallRequest request);

}  // namespace orangutan::bootstrap
