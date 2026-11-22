#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include <asio/steady_timer.hpp>
#include <oran/async/channel.hpp>
#include <oran/async/task_group.hpp>
#include <oran/bootstrap/agent_session.hpp>
#include <oran/bootstrap/background_tasks.hpp>
#include <oran/tool/registry.hpp>

namespace orangutan::bootstrap {

[[nodiscard]] bool task_terminal(TaskState state);
[[nodiscard]] bool team_task_active(TeamTaskState state);
[[nodiscard]] core::Result<permission::RuleSet> copy_child_rules(std::span<const permission::Rule> rules);

struct BackgroundTasks::Impl : std::enable_shared_from_this<Impl> {
  struct Child {
    permission::RuleSet parent_rules;
    std::unique_ptr<AgentSession> session;
  };
  struct Team {
    struct Member {
      TeamMemberSnapshot snapshot;
      std::string instructions{};
      std::shared_ptr<Child> child{};
      bool initialized{false};
    };
    struct Work {
      TeamTaskSnapshot snapshot;
      std::string prompt{};
      std::optional<core::TurnId> parent_turn;
    };
    struct Message {
      InboxMessage envelope;
      std::string recipient{};
      bool acknowledged{false};
    };
    TaskOwner owner;
    std::string id;
    std::string goal;
    bool stopped{false};
    std::vector<Member> members{};
    std::vector<Work> work{};
    std::vector<Message> messages{};

    [[nodiscard]] bool has_active_work(std::string_view member = {}) const;
    [[nodiscard]] std::size_t pending_messages() const;
  };
  struct Job {
    explicit Job(asio::any_io_executor executor) : deadline{std::move(executor)} {}
    TaskOwner owner;
    std::optional<core::TurnId> parent_turn;
    TaskSnapshot snapshot;
    std::shared_ptr<Child> child{};
    std::optional<async::TaskGroup> group;
    asio::steady_timer deadline;
    std::string prompt{};
    std::optional<core::Result<agent::PromptResult>> outcome;
    std::string result{};
    bool timed_out{false};
    bool claimed{false};
    std::chrono::steady_clock::time_point finished{};
    std::shared_ptr<Team> team{};
  };

  Impl(asio::any_io_executor exec,
       RuntimeAssembly& resources,
       const config::Config& cfg,
       BackgroundTaskOptions opts,
       agent::ToolSchedulerOptions sched);
  asio::any_io_executor executor;
  RuntimeAssembly* assembly;
  const config::Config* config;
  BackgroundTaskOptions options;
  tool::Registry registry;
  agent::ToolScheduler scheduler;
  async::Channel<int> changed;
  std::vector<std::shared_ptr<Job>> jobs{};
  std::vector<std::shared_ptr<Team>> teams{};
  std::size_t active{0};
  bool closed{false};
  bool joining{false};

  void prune();
  [[nodiscard]] bool has_record_capacity(bool reserved = false);
  [[nodiscard]] bool has_capacity(bool reserved = false);
  [[nodiscard]] core::Result<void> enqueue(const std::shared_ptr<Job>& job);
  [[nodiscard]] std::shared_ptr<Job> find(const TaskOwner& owner, std::string_view id);
  void cancel_job(const std::shared_ptr<Job>& job, bool timeout = false);
  static async::Awaitable<core::Result<void>> execute(std::shared_ptr<Job> job);
  static async::Awaitable<void> monitor(std::shared_ptr<Impl> self, std::shared_ptr<Job> job);
  void pump();
  [[nodiscard]] std::shared_ptr<Team> find_team(const TaskOwner& owner);
  [[nodiscard]] core::Result<void>
  create_team(const AgentSessionOptions& parent, tool::TeamCreateRequest request, const tool::DispatchContext& context);
  [[nodiscard]] core::Result<void> add_team_member(const AgentSessionOptions& parent,
                                                   const std::shared_ptr<Team>& team,
                                                   tool::TeamMemberSpec specification,
                                                   const tool::DispatchContext& context);
  [[nodiscard]] core::Result<void>
  submit_team_task(Team& team, tool::TeamTaskCreateRequest request, std::optional<core::TurnId> parent_turn);
  [[nodiscard]] core::Result<void>
  update_team_task(Team& team, tool::TeamTaskUpdateRequest request, std::optional<core::TurnId> parent_turn);
  void stop_team(Team& team);
  void pump_teams();
  void update_team_job(const Job& job);
  [[nodiscard]] core::Result<tool::Output>
  send_team_message(Team& team, std::string sender, tool::TeamMessageRequest request);
  void bind_team_inbox(AgentSessionOptions& options, const std::shared_ptr<Team>& team, std::string member);
};

[[nodiscard]] core::Result<tool::Output>
team_output(const BackgroundTasks::Impl::Team& team, bool automatic_delivery, tool::TeamGetRequest request = {});

}  // namespace orangutan::bootstrap
