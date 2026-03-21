#include "features/automation/runtime.hpp"

#include "features/automation/planner.hpp"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace orangutan::automation {
namespace {

json make_log_entry(const Trigger &trigger, const ExecutionResult &result, std::int64_t started_at, std::string_view status) {
    return {
        {"kind", kind_to_string(trigger.kind)},
        {"automation_id", trigger.automation_id},
        {"agent_key", trigger.agent_key},
        {"name", trigger.name},
        {"started_at", started_at},
        {"status", status},
        {"summary", result.summary},
        {"reply", result.reply},
    };
}

std::string result_body(const ExecutionResult &result) {
    return result.reply.empty() ? result.summary : result.reply;
}

} // namespace

class Runtime::AgentExecutionGate {
public:
    std::mutex mutex;
    std::condition_variable cv;
    std::thread::id owner;
    size_t depth = 0;
};

Runtime::AgentExecutionLease::AgentExecutionLease(std::shared_ptr<AgentExecutionGate> gate)
: gate_(std::move(gate)),
  owner_(std::this_thread::get_id()) {
    std::unique_lock lock(gate_->mutex);
    gate_->cv.wait(lock, [this] {
        return gate_->depth == 0 || gate_->owner == owner_;
    });
    gate_->owner = owner_;
    ++gate_->depth;
}

Runtime::AgentExecutionLease::AgentExecutionLease(AgentExecutionLease &&other) noexcept
: gate_(std::exchange(other.gate_, nullptr)),
  owner_(other.owner_) {}

Runtime::AgentExecutionLease &Runtime::AgentExecutionLease::operator=(AgentExecutionLease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    release();
    gate_ = std::exchange(other.gate_, nullptr);
    owner_ = other.owner_;
    return *this;
}

Runtime::AgentExecutionLease::~AgentExecutionLease() {
    release();
}

void Runtime::AgentExecutionLease::release() noexcept {
    auto gate = std::exchange(gate_, nullptr);
    if (!gate) {
        return;
    }

    std::unique_lock lock(gate->mutex);
    if (gate->owner == owner_ && gate->depth > 0) {
        --gate->depth;
        if (gate->depth == 0) {
            gate->owner = std::thread::id{};
            lock.unlock();
            gate->cv.notify_all();
        }
    }
}

Runtime::Runtime(Store &store)
: store_(store) {}

Runtime::~Runtime() {
    stop();
}

void Runtime::set_executor(Executor executor) {
    std::lock_guard lock(mutex_);
    executor_ = std::move(executor);
}

void Runtime::set_notifier(Notifier notifier) {
    std::lock_guard lock(mutex_);
    notifier_ = std::move(notifier);
}

void Runtime::start() {
    if (running_.exchange(true)) {
        return;
    }

    startup_time_ = to_unix_seconds(Clock::now());
    normalize_state(Clock::now());
    worker_ = std::thread([this] {
        scheduler_loop();
    });
}

void Runtime::stop() {
    const bool was_running = running_.exchange(false);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (!was_running) {
        return;
    }
}

void Runtime::run_pending(TimePoint now) {
    const auto due = collect_due_items(store_.list_tasks(), store_.list_heartbeats(), now, startup_time_);
    for (const auto &task : due.tasks) {
        execute_task(task);
    }
    for (const auto &heartbeat : due.heartbeats) {
        with_agent_execution_lease(this, heartbeat.agent_key, [&] {
            execute_heartbeat(heartbeat);
        });
    }
}

Runtime::AgentExecutionLease Runtime::acquire_agent_execution_lease(const std::string &agent_key) {
    return AgentExecutionLease(get_agent_execution_gate(agent_key));
}

std::vector<TaskSpec> Runtime::list_tasks(const std::string &agent_key) const {
    return store_.list_tasks(agent_key);
}

std::optional<TaskSpec> Runtime::find_task(const std::string &agent_key, const std::string &id_or_name) const {
    return store_.find_task(agent_key, id_or_name);
}

std::string Runtime::save_task(const TaskSpec &task) {
    return store_.upsert_task(task);
}

bool Runtime::remove_task(const std::string &agent_key, const std::string &id_or_name) {
    return store_.remove_task(agent_key, id_or_name);
}

std::string Runtime::run_task_now(const std::string &agent_key, const std::string &id_or_name) {
    const auto task = store_.find_task(agent_key, id_or_name);
    if (!task.has_value()) {
        return "Error: task not found.";
    }
    execute_task(*task, to_unix_seconds(Clock::now()));
    return "Task run queued.";
}

std::vector<HeartbeatSpec> Runtime::list_heartbeats(const std::string &agent_key) const {
    return store_.list_heartbeats(agent_key);
}

std::optional<HeartbeatSpec> Runtime::find_heartbeat(const std::string &agent_key, const std::string &id_or_name) const {
    return store_.find_heartbeat(agent_key, id_or_name);
}

std::string Runtime::save_heartbeat(const HeartbeatSpec &heartbeat_input) {
    HeartbeatSpec heartbeat = heartbeat_input;
    if (!heartbeat.next_due_at.has_value()) {
        heartbeat.next_due_at = plan_next_heartbeat_due(heartbeat, Clock::now());
    }
    return store_.upsert_heartbeat(heartbeat);
}

bool Runtime::remove_heartbeat(const std::string &agent_key, const std::string &id_or_name) {
    return store_.remove_heartbeat(agent_key, id_or_name);
}

bool Runtime::pause_heartbeat(const std::string &agent_key, const std::string &id_or_name, bool paused) {
    const auto heartbeat = store_.find_heartbeat(agent_key, id_or_name);
    if (!heartbeat.has_value()) {
        return false;
    }

    auto next_due_at = heartbeat->next_due_at;
    if (!paused) {
        next_due_at = plan_next_heartbeat_due(*heartbeat, Clock::now());
    }
    store_.update_heartbeat_run_state(heartbeat->id, heartbeat->last_run_at, next_due_at, heartbeat->last_status, paused);
    return true;
}

std::string Runtime::run_heartbeat_now(const std::string &agent_key, const std::string &id_or_name) {
    const auto heartbeat = store_.find_heartbeat(agent_key, id_or_name);
    if (!heartbeat.has_value()) {
        return "Error: heartbeat not found.";
    }
    with_agent_execution_lease(this, heartbeat->agent_key, [&] {
        execute_heartbeat(*heartbeat, to_unix_seconds(Clock::now()));
    });
    return "Heartbeat run queued.";
}

std::vector<InboxItem> Runtime::list_inbox(const std::string &agent_key) const {
    return store_.list_inbox(agent_key);
}

bool Runtime::ack_inbox(const std::string &agent_key, const std::string &id) {
    return store_.ack_inbox(agent_key, id);
}

void Runtime::clear_inbox(const std::string &agent_key) {
    store_.clear_inbox(agent_key);
}

Store &Runtime::store() noexcept {
    return store_;
}

const Store &Runtime::store() const noexcept {
    return store_;
}

void Runtime::scheduler_loop() {
    while (running_.load()) {
        run_pending(Clock::now());
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(1), [this] {
            return !running_.load();
        });
    }
}

void Runtime::normalize_state(TimePoint now) {
    const auto now_seconds = to_unix_seconds(now);

    for (const auto &task : store_.list_tasks()) {
        if (task.schedule.kind != TaskScheduleKind::at) {
            continue;
        }
        const auto scheduled = parse_absolute_time(task.schedule.value);
        if (!scheduled.has_value()) {
            continue;
        }
        if (*scheduled < now_seconds && !task.last_run_at.has_value()) {
            store_.update_task_run_state(task.id, task.last_run_at, "missed", false);
        }
    }

    for (const auto &heartbeat : store_.list_heartbeats()) {
        if (!heartbeat.enabled) {
            continue;
        }
        auto next_due = heartbeat.next_due_at;
        if (!next_due.has_value() || *next_due < now_seconds) {
            next_due = plan_next_heartbeat_due(heartbeat, now);
        }
        store_.update_heartbeat_run_state(heartbeat.id, heartbeat.last_run_at, next_due, heartbeat.last_status, heartbeat.paused);
    }
}

std::shared_ptr<Runtime::AgentExecutionGate> Runtime::get_agent_execution_gate(const std::string &agent_key) {
    const auto key = agent_key.empty() ? std::string("default") : agent_key;
    std::lock_guard lock(agent_execution_gates_mutex_);
    auto &entry = agent_execution_gates_[key];
    auto gate = entry.lock();
    if (!gate) {
        gate = std::make_shared<AgentExecutionGate>();
        entry = gate;
    }
    return gate;
}

Runtime::CompletedExecution Runtime::execute_trigger(const Trigger &trigger, std::int64_t started_at) {
    auto executor = Executor{};
    auto notifier = Notifier{};
    {
        std::lock_guard lock(mutex_);
        executor = executor_;
        notifier = notifier_;
    }

    const auto run_id = store_.insert_run(RunRecord{
        .kind = trigger.kind,
        .automation_id = trigger.automation_id,
        .agent_key = trigger.agent_key,
        .automation_name = trigger.name,
        .started_at = started_at,
        .status = "running",
    });

    CompletedExecution completed;
    ExecutionResult result;
    if (executor) {
        result = executor(trigger);
    } else {
        result.success = false;
        result.summary = "No automation executor configured";
    }
    completed.result = result;
    completed.finished_at = to_unix_seconds(Clock::now());
    completed.status = result.success ? std::string("completed") : std::string("failed");

    const auto body = result_body(result);

    if (trigger.delivery.mode == DeliveryMode::silent) {
        if (!result.workspace_root.empty()) {
            completed.log_path = log_writer_.append(result.workspace_root, make_log_entry(trigger, result, started_at, completed.status));
        }
    } else if (trigger.delivery.targets.empty()) {
        completed.delivery_status = "inbox";
        record_delivery_failure(trigger, run_id, "Notification target missing", body);
    } else {
        completed.delivery_status = "notified";
        for (const auto &target : trigger.delivery.targets) {
            if (target == "inbox" || target == "cli" || target == "web") {
                record_delivery_failure(trigger, run_id, trigger.name, body);
                continue;
            }
            if (!notifier) {
                completed.delivery_status = "notify_failed";
                record_delivery_failure(trigger, run_id, trigger.name, body);
                continue;
            }
            if (const auto error = notifier(target, body); error.has_value()) {
                completed.delivery_status = "notify_failed";
                record_delivery_failure(trigger, run_id, trigger.name, *error + "\n" + body);
            }
        }
    }

    store_.complete_run(run_id, completed.status, result.summary, completed.delivery_status, completed.log_path, completed.finished_at);
    return completed;
}

void Runtime::execute_task(const TaskSpec &task, std::optional<std::int64_t> forced_timestamp) {
    const auto started_at = forced_timestamp.value_or(to_unix_seconds(Clock::now()));
    const Trigger trigger{
        .kind = Kind::task,
        .automation_id = task.id,
        .agent_key = task.agent_key,
        .name = task.name,
        .prompt = task.prompt,
        .delivery = task.delivery,
    };

    const auto completed = execute_trigger(trigger, started_at);
    store_.update_task_run_state(task.id, completed.finished_at, completed.status, task.schedule.kind == TaskScheduleKind::cron);
}

void Runtime::execute_heartbeat(const HeartbeatSpec &heartbeat, std::optional<std::int64_t> forced_timestamp) {
    const auto started_at = forced_timestamp.value_or(to_unix_seconds(Clock::now()));
    const Trigger trigger{
        .kind = Kind::heartbeat,
        .automation_id = heartbeat.id,
        .agent_key = heartbeat.agent_key,
        .name = heartbeat.name,
        .prompt = heartbeat.prompt,
        .delivery = heartbeat.delivery,
    };

    HeartbeatSpec updated = heartbeat;
    const auto completed = execute_trigger(trigger, started_at);
    updated.last_run_at = completed.finished_at;
    updated.next_due_at = plan_next_heartbeat_due(updated, Clock::now());
    store_.update_heartbeat_run_state(heartbeat.id, updated.last_run_at, updated.next_due_at, completed.status, heartbeat.paused);
}

void Runtime::record_delivery_failure(const Trigger &trigger, std::string_view run_id, std::string_view title, std::string_view body) {
    (void)store_.insert_inbox(InboxItem{
        .agent_key = trigger.agent_key,
        .source_kind = kind_to_string(trigger.kind),
        .source_run_id = std::string(run_id),
        .title = std::string(title),
        .body = std::string(body),
        .created_at = to_unix_seconds(Clock::now()),
    });
}

} // namespace orangutan::automation
