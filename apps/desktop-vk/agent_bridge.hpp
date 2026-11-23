#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "theme.hpp"

namespace orangutan::vkui {

struct ChatMessage {
  std::string id;
  bool is_user{false};
  CompanionId speaker{CompanionId::oran};
  std::string text;
  bool streaming{false};
};

struct SubTaskCard {
  std::string id;
  CompanionId assignee{CompanionId::oran};
  std::string title;
  std::string status;  // "running" | "done" | "waiting"
  std::string detail;
};

struct ConversationSummary {
  std::string id;
  std::string title;
};

struct MemoryNoteView {
  std::string id;
  std::string scope;  // "personal" | "shared"
  std::string title;
  std::string body;
};

struct ReminderView {
  std::string id;
  std::string title;
  std::int64_t due_at_ms{0};
  bool completed{false};
};

struct PendingApprovalView {
  std::string id;
  std::string action_name;
  std::string target;
  std::string reason;
};

struct AgentSnapshot {
  bool demo_mode{true};
  std::string model_name{"orangutan-local-demo"};
  std::string workspace_path;
  std::string state_dir;
  std::string permission_policy{"ask"};  // "ask" | "allow" | "deny"

  CompanionId lead_companion{CompanionId::oran};
  CompanionMood active_mood{CompanionMood::idle};
  std::string status_line{"准备好了，随时可以开始"};
  bool busy{false};

  std::string active_conversation_id;
  std::vector<ConversationSummary> conversations;
  std::vector<ChatMessage> messages;
  std::vector<SubTaskCard> subtasks;
  std::vector<MemoryNoteView> notes;
  std::vector<ReminderView> reminders;
  std::optional<PendingApprovalView> pending_approval;
};

struct AgentBridgeOptions {
  std::string workspace_path{"."};
  std::string state_dir;
  std::string config_path;
  bool demo_mode{true};
};

/// Direct in-process bridge between the pure Vulkan UI and Orangutan's C++26
/// `oran-bootstrap` Personal Agent runtime (`AgentSession`, `Fts5Backend`,
/// `ReminderStore`, interactive `permission_ask_rendered` broker).
class AgentBridge {
public:
  AgentBridge();
  ~AgentBridge();

  AgentBridge(const AgentBridge&) = delete;
  AgentBridge& operator=(const AgentBridge&) = delete;

  bool start(AgentBridgeOptions opts, std::string& error_out);
  void stop();

  [[nodiscard]] AgentSnapshot snapshot() const;

  void set_lead_companion(CompanionId id);
  void set_permission_policy(std::string policy);
  void new_conversation();
  void select_conversation(std::string id);
  void send_message(std::string text);
  void stop_active_turn();
  void resolve_approval(std::string approval_id, bool approve);
  void add_memory_note(std::string title, std::string body, std::string scope = "personal");
  void delete_memory_note(std::string id, std::string scope = "personal");
  void add_reminder(std::string title, std::int64_t due_at_ms);
  void toggle_reminder(std::string id, bool completed);
  void delete_reminder(std::string id);

  /// Populates a realistic task-split demo state (used only for headless screenshot showcase).
  void seed_showcase_state();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orangutan::vkui
