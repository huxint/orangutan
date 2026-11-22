#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <asio/any_io_executor.hpp>
#include <oran/async/awaitable_fwd.hpp>
#include <oran/bootstrap/agent-team.hpp>
#include <oran/core/result.hpp>
#include <oran/core/time.hpp>
#include <oran/core/turn_id.hpp>
#include <oran/tool/team.hpp>

namespace orangutan::config {
class Config;
}
namespace orangutan::agent {
class ToolScheduler;
}
namespace orangutan::tool {
class Registry;
struct AgentRunRequest;
struct DispatchContext;
struct Output;
}  // namespace orangutan::tool

namespace orangutan::bootstrap {
class RuntimeAssembly;
struct AgentSessionOptions;

enum class TaskState {
  queued,
  running,
  cancelling,
  succeeded,
  failed,
  cancelled
};

struct TaskOwner {
  core::TurnId session_id{};
  std::string scope_key;
  std::string agent_key;
  std::string identity;
  friend bool operator==(const TaskOwner&, const TaskOwner&) = default;
};

struct TaskSnapshot {
  std::string task_id;
  std::string agent_key;
  std::string label;
  TaskState state{TaskState::queued};
  core::Time created_at{};
  core::Time updated_at{};
  bool automatic_delivery{false};
  bool acknowledged{false};
  std::string result;
  std::size_t result_offset{0};
  std::size_t result_bytes{0};
  std::optional<std::size_t> next_offset{};
  bool result_truncated{false};
  std::optional<core::ErrorKind> error_kind{};
  std::string team_id;
  std::string team_task_key;
  std::size_t team_attempt{0};
};

struct BackgroundTaskOptions {
  std::size_t max_running{4};
  std::size_t max_queued{16};
  std::size_t max_records{64};
  std::size_t max_result_bytes{65536};
  std::chrono::milliseconds timeout{std::chrono::minutes{15}};
  std::chrono::milliseconds retention{std::chrono::hours{1}};
  /// Only a host with an actual completion wake/poll path may enable this.
  bool automatic_delivery{false};
  /// Adds persistent specialists and a leader-reviewed task board.
  bool enable_teams{false};
};

/// Process-local child jobs and shared tool resources. Every method runs on the
/// supplied coordinating strand. The host retains provider/config/assembly and
/// awaits shutdown before destroying them or this service. Parent turns may end.
class BackgroundTasks {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<BackgroundTasks>> create(asio::any_io_executor executor,
                                                                             RuntimeAssembly& assembly,
                                                                             const config::Config& config,
                                                                             BackgroundTaskOptions options = {});
  ~BackgroundTasks();
  BackgroundTasks(const BackgroundTasks&) = delete;
  BackgroundTasks& operator=(const BackgroundTasks&) = delete;

  [[nodiscard]] bool matches(const AgentSessionOptions& parent) const;
  [[nodiscard]] tool::Registry& registry();
  [[nodiscard]] agent::ToolScheduler& scheduler();
  /// Called after AgentRun validates its request; admission is host-owned.
  [[nodiscard]] core::Result<TaskSnapshot>
  start(const AgentSessionOptions& parent, tool::AgentRunRequest request, const tool::DispatchContext& context);
  /// Callers supply the trusted owner; tools cannot select scope or identity.
  [[nodiscard]] core::Result<TaskSnapshot>
  get(const TaskOwner& owner, std::string_view id, std::size_t offset = 0, std::size_t max_bytes = 8192);
  [[nodiscard]] core::Result<TaskSnapshot> cancel(const TaskOwner& owner, std::string_view id);
  [[nodiscard]] std::vector<TaskSnapshot> list(const TaskOwner& owner);
  [[nodiscard]] std::optional<std::string> next_completion(const TaskOwner& owner);
  [[nodiscard]] core::Result<bool> claim_completion(const TaskOwner& owner, std::string_view id);
  void finish_completion(const TaskOwner& owner, std::string_view id, bool committed);
  void cancel_owner(const TaskOwner& owner, std::optional<core::TurnId> turn = std::nullopt);
  [[nodiscard]] bool teams_enabled() const;
  /// Scoped leader commands. Host calls use the same validation as native tools.
  [[nodiscard]] core::Result<tool::Output>
  team_command(const AgentSessionOptions& parent, tool::TeamCommand command, const tool::DispatchContext& context);
  [[nodiscard]] core::Result<TeamSnapshot> get_team(const TaskOwner& owner);
  [[nodiscard]] async::Awaitable<core::Result<void>> shutdown();

  struct Impl;
  class PrivateTag {
    PrivateTag() = default;
    friend class BackgroundTasks;
  };
  BackgroundTasks(std::shared_ptr<Impl> impl, PrivateTag);

private:
  std::shared_ptr<Impl> impl_;
};

[[nodiscard]] TaskOwner task_owner(const AgentSessionOptions& options);
[[nodiscard]] tool::Output task_output(const TaskSnapshot& snapshot);
}  // namespace orangutan::bootstrap
