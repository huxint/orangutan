#pragma once

#include <cstddef>

namespace orangutan::agent {
class ToolScheduler;
}
namespace orangutan::tool {
struct DispatchContext;
class Registry;
}  // namespace orangutan::tool

namespace orangutan::bootstrap {

struct AgentSessionOptions;
void bind_task_tools(tool::DispatchContext& context, const AgentSessionOptions& options);

/// Bind child runs to the parent's strand and prompt-local admission count.
void bind_child_agents(tool::DispatchContext& context,
                       const AgentSessionOptions& options,
                       tool::Registry& registry,
                       agent::ToolScheduler& scheduler,
                       std::size_t& child_runs);

}  // namespace orangutan::bootstrap
