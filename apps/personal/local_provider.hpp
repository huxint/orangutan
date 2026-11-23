#pragma once

#include <atomic>
#include <string>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/provider/system.hpp>
#include <oran/provider/types.hpp>

namespace orangutan::personal {

/// Zero-credential local Personal Agent provider for offline exploration and
/// end-to-end verification. Keeps the tool surface minimal: composes workspace
/// inspection via `ShellExec`, file updates via `FileWrite`, and durable notes
/// via `MemoryRemember` through the real `AgentSession`, permission rules,
/// approval broker, and SQLite stores.
class LocalDemoProvider final : public provider::System {
public:
  LocalDemoProvider() = default;
  ~LocalDemoProvider() override = default;

  [[nodiscard]] async::Awaitable<core::Result<provider::Response>>
  send(provider::Request request, provider::ModelTarget target, provider::EventSink* sink = nullptr) const override;

private:
  mutable std::atomic<std::uint64_t> call_seq_{0};
};

}  // namespace orangutan::personal
