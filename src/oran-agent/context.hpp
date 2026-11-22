#pragma once

#include <functional>
#include <vector>

#include <oran/agent/context.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/working_context.hpp>

namespace orangutan::agent::detail {

using SummarySender = std::function<async::Awaitable<core::Result<provider::Response>>(provider::Request)>;

struct ContextView {
  core::WorkingContext checkpoint;
  std::vector<core::Message> messages;
  bool deferred{};
};

[[nodiscard]] std::vector<core::Message> context_messages(const ContextView& view);
[[nodiscard]] async::Awaitable<core::Result<void>> fit_context(ContextView& view,
                                                               const provider::Request& frame,
                                                               const ContextOptions& options,
                                                               const SummarySender& send);

}  // namespace orangutan::agent::detail
