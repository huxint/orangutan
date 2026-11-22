#include "_impl/background-tasks.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace orangutan::bootstrap {

std::size_t BackgroundTasks::Impl::Team::pending_messages() const {
  return static_cast<std::size_t>(
      std::ranges::count_if(messages, [](const auto& message) { return !message.acknowledged; }));
}

core::Result<tool::Output>
BackgroundTasks::Impl::send_team_message(Team& team, std::string sender, tool::TeamMessageRequest request) {
  if (auto valid = tool::validate_team_command(request); !valid)
    return std::unexpected(std::move(valid).error());
  if (closed || team.stopped)
    return std::unexpected(core::Error{core::ErrorKind::conflict, "team messaging is closed"});
  const auto recipient = std::ranges::find_if(team.members, [&request](const auto& member) {
    return member.snapshot.name == request.recipient;
  });
  const auto task =
      std::ranges::find_if(team.work, [&request](const auto& work) { return work.snapshot.key == request.task_key; });
  if (recipient == team.members.end() || task == team.work.end())
    return std::unexpected(core::Error::not_found("team recipient or task is unavailable"));
  if (request.recipient == sender)
    return std::unexpected(core::Error::invalid_argument("send team messages to another member"));
  if (sender != "leader" && (task->snapshot.member != sender || task->snapshot.state != TeamTaskState::running))
    return std::unexpected(core::Error::permission_denied("members can message only about their active task"));
  const auto pending = std::ranges::count_if(team.messages, [&request](const auto& message) {
    return message.recipient == request.recipient && !message.acknowledged;
  });
  if (team.messages.size() >= 64 || pending >= 16)
    return std::unexpected(core::Error{core::ErrorKind::mailbox_overflowed, "team message limit reached"});
  const auto id = std::to_string(team.messages.size() + 1);
  auto body = nlohmann::json{{"task", request.task_key},
                             {"attempt", task->snapshot.attempt},
                             {"message", std::move(request.message)}}
                  .dump();
  if (body.size() > 8192)
    return std::unexpected(core::Error::invalid_argument("encoded team message exceeds the inbox byte bound"));
  team.messages.push_back(
      {.envelope = {.id = id, .sender = std::move(sender), .text = std::move(body)}, .recipient = request.recipient});
  auto receipt = nlohmann::json{
      {"kind", "team_message"},
      {"message_id", id},
      {"recipient", request.recipient},
      {"state", "queued"},
      {"wakes_idle_member",
       false}}.dump();
  return tool::Output{.text =
                          receipt + "\nQueued for the recipient's next model request; this does not imply it was read.",
                      .data_json = std::move(receipt)};
}

void BackgroundTasks::Impl::bind_team_inbox(AgentSessionOptions& options,
                                            const std::shared_ptr<Team>& team,
                                            std::string member) {
  const auto weak_team = std::weak_ptr{team};
  const auto weak_self = weak_from_this();
  options.team_send = [weak_self, weak_team, member](tool::TeamMessageRequest request) -> core::Result<tool::Output> {
    auto self = weak_self.lock();
    auto value = weak_team.lock();
    if (!self || !value)
      return std::unexpected(core::Error::not_found("team is unavailable"));
    return self->send_team_message(*value, member, std::move(request));
  };
  auto inbox = std::make_shared<SessionInbox>();
  inbox->read = [weak_team, member]() -> core::Result<std::vector<InboxMessage>> {
    auto value = weak_team.lock();
    if (!value)
      return std::unexpected(core::Error::not_found("team inbox is unavailable"));
    std::vector<InboxMessage> messages;
    for (const auto& message : value->messages)
      if (message.recipient == member && !message.acknowledged)
        messages.push_back(message.envelope);
    return messages;
  };
  inbox->acknowledge = [weak_team, member](std::span<const std::string> ids) {
    if (auto value = weak_team.lock())
      for (auto& message : value->messages)
        if (message.recipient == member && std::ranges::contains(ids, message.envelope.id))
          message.acknowledged = true;
  };
  options.inbox = std::move(inbox);
}
}  // namespace orangutan::bootstrap
