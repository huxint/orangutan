#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <oran/async/awaitable_fwd.hpp>
#include <oran/core/result.hpp>
#include <oran/storage/pool.hpp>

namespace orangutan::personal {

struct Reminder {
  std::string id;
  std::string title;
  std::int64_t due_at_ms{0};
  bool completed{false};
  bool notified{false};
  std::int64_t created_at_ms{0};

  friend bool operator==(const Reminder&, const Reminder&) = default;
};

/// Durable SQLite personal reminder store built on `oran-storage`.
class ReminderStore {
public:
  explicit ReminderStore(storage::Pool& pool) noexcept;

  [[nodiscard]] async::Awaitable<core::Result<void>> migrate();
  [[nodiscard]] async::Awaitable<core::Result<std::vector<Reminder>>> list();
  [[nodiscard]] async::Awaitable<core::Result<Reminder>> add(Reminder reminder);
  [[nodiscard]] async::Awaitable<core::Result<void>> mark_completed(std::string id, bool completed = true);
  [[nodiscard]] async::Awaitable<core::Result<void>> remove(std::string id);
  /// Mark all due, uncompleted, unnotified reminders as notified and return them.
  [[nodiscard]] async::Awaitable<core::Result<std::vector<Reminder>>> claim_due(std::int64_t now_ms);

private:
  storage::Pool* pool_{nullptr};
};

}  // namespace orangutan::personal
