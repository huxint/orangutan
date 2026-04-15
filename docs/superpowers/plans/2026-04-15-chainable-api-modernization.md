# Chainable API Modernization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unify all chainable APIs across the Orangutan project using C++23 deducing `this`, standardize error handling with `std::expected`, and fix clang-tidy warnings.

**Architecture:** Bottom-up refactoring from type layer through services. Each module is modernized in dependency order to ensure downstream consumers see updated APIs. The canonical pattern uses deducing `this` with `std::forward`, `[[nodiscard]]` terminals, and `std::expected<T, std::string>` for fallible operations.

**Tech Stack:** C++23, Catch2 3.7.1, xmake, clang-tidy

---

## Canonical Pattern Reference

The gold standard is `src/providers/provider.hpp` `RequestBuilder`:

```cpp
class RequestBuilder {
public:
    auto route(this auto &&self, ProviderRoute route) -> decltype(auto) {
        self.route_ = std::move(route);
        return std::forward<decltype(self)>(self);
    }

    auto system(this auto &&self, std::string_view prompt) -> decltype(auto) {
        self.request_.system_prompt = std::string(prompt);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    provider_sender send() const;
};
```

Rules:
- Chainable methods: `(this auto &&self, ...) -> decltype(auto)` + `return std::forward<decltype(self)>(self);`
- Terminal methods: `const` where possible, marked `[[nodiscard]]`
- Fallible terminals: return `std::expected<T, std::string>`
- Error strings: all lowercase, no trailing period, concise

---

## File Structure

Target file map:

- Modify: `src/types/message.hpp`
- Modify: `src/types/tool-def.hpp`
- Modify: `src/tools/registry/tool-spec-builder.hpp`
- Modify: `src/tools/registry/tool-dispatch.hpp`
- Modify: `src/tools/registry/contextual-tool-group.hpp`
- Modify: `src/channel/qq/qq-message-builder.hpp`
- Modify: `src/automation/builder.hpp`
- Modify: `src/automation/builder.cpp`
- Modify: `src/agent/agent-loop.hpp`
- Modify: `src/skills/skill-loader.hpp`
- Modify: `src/skills/skill-loader.cpp`
- Modify: `src/web/web-server.hpp`
- Modify: `src/web/web-server.cpp`
- Modify: `src/coordinator/coordinator-manager.hpp`
- Modify: `src/coordinator/coordinator-manager.cpp`
- Modify: `tests/automation/automation-model-test.cpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `tests/channel/qq/qq-message-builder-test.cpp`
- Modify: `tests/agent/agent-loop-test.cpp`

Build note:
- `xmake/targets.lua` already globs all `src/**/*.cpp`
- `xmake/tests.lua` already globs all `tests/**/*.cpp`
- No build-file changes should be required

---

### Task 1: Migrate Conversation to deducing `this` (Layer 1)

**Files:**

- Modify: `src/types/message.hpp`
- Test: `tests/types/` (existing compile tests)

- [ ] **Step 1: Convert `Conversation::append()` from `return *this` to deducing `this`**

Before (`src/types/message.hpp` lines 109–112):

```cpp
auto append(Message msg) -> Conversation & {
    messages_.push_back(std::move(msg));
    return *this;
}
```

After:

```cpp
auto append(this auto &&self, Message msg) -> decltype(auto) {
    self.messages_.push_back(std::move(msg));
    return std::forward<decltype(self)>(self);
}
```

- [ ] **Step 2: Convert `Conversation::user()` and `Conversation::assistant()` to deducing `this`**

Before (`src/types/message.hpp` lines 115–123):

```cpp
template <typename... Args>
auto user(Args &&...args) -> Conversation & {
    return emplace(base::role::user, std::forward<Args>(args)...);
}

template <typename... Args>
auto assistant(Args &&...args) -> Conversation & {
    return emplace(base::role::assistant, std::forward<Args>(args)...);
}
```

After:

```cpp
template <typename... Args>
auto user(this auto &&self, Args &&...args) -> decltype(auto) {
    return emplace(std::forward<decltype(self)>(self), base::role::user, std::forward<Args>(args)...);
}

template <typename... Args>
auto assistant(this auto &&self, Args &&...args) -> decltype(auto) {
    return emplace(std::forward<decltype(self)>(self), base::role::assistant, std::forward<Args>(args)...);
}
```

- [ ] **Step 3: Convert the private `emplace()` helper to accept forwarded self**

Before (`src/types/message.hpp` lines 148–154):

```cpp
template <typename... Args>
auto emplace(base::role role, Args &&...args) -> Conversation & {
    auto msg = Message(role);
    (emplace_one(msg, std::forward<Args>(args)), ...);
    messages_.push_back(std::move(msg));
    return *this;
}
```

After:

```cpp
template <typename... Args>
static auto emplace(auto &&self, base::role role, Args &&...args) -> decltype(self) {
    auto msg = Message(role);
    (emplace_one(msg, std::forward<Args>(args)), ...);
    self.messages_.push_back(std::move(msg));
    return std::forward<decltype(self)>(self);
}
```

- [ ] **Step 4: Build and test**

```bash
xmake build test-types
ctest --test-dir build -R test-types --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add src/types/message.hpp
git commit -m "refactor: migrate Conversation to deducing this"
```

---

### Task 2: Fix clang-tidy warnings in types/ (Layer 1)

**Files:**

- Modify: `src/types/tool-def.hpp`

- [ ] **Step 1: Add explicit underlying type to `response_stop_reason` enum**

Before (`src/types/tool-def.hpp` lines 11–16):

```cpp
enum class response_stop_reason {
    end_turn,
    tool_use,
    max_tokens,
    unknown,
};
```

After:

```cpp
enum class response_stop_reason : std::uint8_t {
    end_turn,
    tool_use,
    max_tokens,
    unknown,
};
```

- [ ] **Step 2: Add `#include <cstdint>` to includes**

Before (`src/types/tool-def.hpp` lines 1–7):

```cpp
#pragma once

#include "types/content.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
```

After:

```cpp
#pragma once

#include "types/content.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
```

Note: also reorder includes to follow the project's include order convention (standard library before third-party).

- [ ] **Step 3: Build to verify**

```bash
xmake build test-types
```

- [ ] **Step 4: Commit**

```bash
git add src/types/tool-def.hpp
git commit -m "fix: add explicit uint8_t underlying type to response_stop_reason"
```

---

### Task 3: Migrate ToolSpecBuilder to deducing `this` + std::expected (Layer 2)

**Files:**

- Modify: `src/tools/registry/tool-spec-builder.hpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`
- Modify: `src/tools/registry/contextual-tool-group.hpp` (caller of `build()`)

- [ ] **Step 1: Add `#include <expected>` to tool-spec-builder.hpp and convert all chainable methods**

Before (representative, `src/tools/registry/tool-spec-builder.hpp` lines 20–23):

```cpp
ToolSpecBuilder &description(std::string value) {
    tool_.definition.description = std::move(value);
    return *this;
}
```

After:

```cpp
auto description(this auto &&self, std::string value) -> decltype(auto) {
    self.tool_.definition.description = std::move(value);
    return std::forward<decltype(self)>(self);
}
```

Apply the same pattern to all chainable methods: `input_schema()`, `read_only()`, `deferred()`, `check_permissions()`, `execute()`, `execute_rich()`.

- [ ] **Step 2: Change `build()` to return `std::expected<Tool, std::string>`**

Before (`src/tools/registry/tool-spec-builder.hpp` lines 57–63):

```cpp
[[nodiscard]]
Tool build() const {
    if (!has_execute_ && !has_execute_rich_) {
        throw std::invalid_argument("tool spec builder requires execute or execute_rich");
    }
    return tool_;
}
```

After:

```cpp
[[nodiscard]]
auto build() const -> std::expected<Tool, std::string> {
    if (!has_execute_ && !has_execute_rich_) {
        return std::unexpected("tool spec builder requires execute or execute_rich");
    }
    return tool_;
}
```

Remove `#include <stdexcept>`, add `#include <expected>`.

- [ ] **Step 3: Update `ContextualToolGroup::register_into()` to handle `std::expected` from `spec.build()`**

Before (`src/tools/registry/contextual-tool-group.hpp` lines 85–87):

```cpp
for (const auto &spec : specs_) {
    registry.register_tool(spec.build());
}
```

After:

```cpp
for (const auto &spec : specs_) {
    auto result = spec.build();
    if (result.has_value()) {
        registry.register_tool(std::move(result).value());
    }
}
```

- [ ] **Step 4: Update test callers that use `build()` directly**

All test sites that call `.build()` on a `ToolSpecBuilder` must now handle `std::expected`. For valid builds:

Before:

```cpp
auto tool = builder.build();
```

After:

```cpp
auto tool = builder.build();
REQUIRE(tool.has_value());
// use *tool or tool.value()
```

For expected-failure tests:

```cpp
auto result = builder.build();
CHECK_FALSE(result.has_value());
CHECK(result.error().contains("requires execute"));
```

- [ ] **Step 5: Build and test**

```bash
xmake build test-tools
ctest --test-dir build -R test-tool --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add src/tools/registry/tool-spec-builder.hpp src/tools/registry/contextual-tool-group.hpp tests/tools/registry/tool-registry-test.cpp
git commit -m "refactor: migrate ToolSpecBuilder to deducing this with std::expected"
```

---

### Task 4: Migrate ToolDispatch to deducing `this` + std::expected (Layer 2)

**Files:**

- Modify: `src/tools/registry/tool-dispatch.hpp`
- Modify: `tests/tools/registry/tool-registry-test.cpp`

- [ ] **Step 1: Add `#include <expected>` and convert all chainable methods to deducing `this`**

Before (representative, `src/tools/registry/tool-dispatch.hpp` lines 27–30):

```cpp
ToolDispatch &op_field(std::string field_name) {
    op_field_ = std::move(field_name);
    return *this;
}
```

After:

```cpp
auto op_field(this auto &&self, std::string field_name) -> decltype(auto) {
    self.op_field_ = std::move(field_name);
    return std::forward<decltype(self)>(self);
}
```

Apply the same to: `unknown_op_error()`, `unknown_op_error_formatter()`, `missing_op_error_formatter()`, `on()`.

- [ ] **Step 2: Change `run()` to return `std::expected<Response, std::string>`**

Before (`src/tools/registry/tool-dispatch.hpp` lines 52–71):

```cpp
[[nodiscard]]
Response run(const nlohmann::json &input) const {
    if (!input.contains(op_field_)) {
        const auto message = missing_op_error_formatter_ != nullptr ? missing_op_error_formatter_(op_field_) : "missing required field: " + op_field_;
        return {.message = message, .is_error = true};
    }

    if (!input.at(op_field_).is_string()) {
        return {.message = "invalid type for field: " + op_field_, .is_error = true};
    }

    const auto op = input.at(op_field_).get<std::string>();
    const auto it = handlers_.find(op);
    if (it == handlers_.end()) {
        const auto message = unknown_op_error_formatter_ != nullptr ? unknown_op_error_formatter_(op) : unknown_op_error_;
        return {.message = message, .is_error = true};
    }

    return it->second(input);
}
```

After:

```cpp
[[nodiscard]]
auto run(const nlohmann::json &input) const -> std::expected<Response, std::string> {
    if (!input.contains(op_field_)) {
        return std::unexpected(missing_op_error_formatter_ != nullptr ? missing_op_error_formatter_(op_field_) : "missing required field: " + op_field_);
    }

    if (!input.at(op_field_).is_string()) {
        return std::unexpected("invalid type for field: " + op_field_);
    }

    const auto op = input.at(op_field_).get<std::string>();
    if (!handlers_.contains(op)) {
        return std::unexpected(unknown_op_error_formatter_ != nullptr ? unknown_op_error_formatter_(op) : unknown_op_error_);
    }

    return handlers_.at(op)(input);
}
```

Note: also replace `handlers_.find(op)` / `it == handlers_.end()` with `handlers_.contains(op)` / `handlers_.at(op)`.

- [ ] **Step 3: Update all callers of `run()` in tests and production code**

Before (caller pattern):

```cpp
auto result = dispatch.run(input);
if (result.is_error) { ... }
```

After:

```cpp
auto result = dispatch.run(input);
if (!result.has_value()) {
    // result.error() contains the message
}
// result->message contains the success message
```

- [ ] **Step 4: Build and test**

```bash
xmake build test-tools
ctest --test-dir build -R test-tool --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add src/tools/registry/tool-dispatch.hpp tests/tools/registry/tool-registry-test.cpp
git commit -m "refactor: migrate ToolDispatch to deducing this with std::expected"
```

---

### Task 5: Migrate ContextualToolGroup to deducing `this` (Layer 2)

**Files:**

- Modify: `src/tools/registry/contextual-tool-group.hpp`

- [ ] **Step 1: Convert `when()`, `require_*()`, and `add()` to deducing `this`**

Before (`src/tools/registry/contextual-tool-group.hpp` lines 46–72):

```cpp
ContextualToolGroup &when(Gate gate) {
    gates_.push_back(std::move(gate));
    return *this;
}

ContextualToolGroup &require_automation_runtime() {
    return when([](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::automation_runtime, ctx);
    });
}

ContextualToolGroup &require_automation_service() {
    return when([](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::automation_service, ctx);
    });
}

ContextualToolGroup &require_channel_origin(base::origin origin) {
    return when([origin](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::channel_origin, ctx, origin);
    });
}

ContextualToolGroup &add(ToolSpecBuilder spec) {
    specs_.push_back(std::move(spec));
    return *this;
}
```

After:

```cpp
auto when(this auto &&self, Gate gate) -> decltype(auto) {
    self.gates_.push_back(std::move(gate));
    return std::forward<decltype(self)>(self);
}

auto require_automation_runtime(this auto &&self) -> decltype(auto) {
    return std::forward<decltype(self)>(self).when([](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::automation_runtime, ctx);
    });
}

auto require_automation_service(this auto &&self) -> decltype(auto) {
    return std::forward<decltype(self)>(self).when([](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::automation_service, ctx);
    });
}

auto require_channel_origin(this auto &&self, base::origin origin) -> decltype(auto) {
    return std::forward<decltype(self)>(self).when([origin](const ToolRuntimeContext &ctx) {
        return detail::ContextGateEvaluator::evaluate(context_gate::channel_origin, ctx, origin);
    });
}

auto add(this auto &&self, ToolSpecBuilder spec) -> decltype(auto) {
    self.specs_.push_back(std::move(spec));
    return std::forward<decltype(self)>(self);
}
```

Note: `register_into()` stays `void` — it is a terminal side-effecting operation, not a chainable setter.

- [ ] **Step 2: Build and test**

```bash
xmake build test-tools
ctest --test-dir build -R test-tool --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add src/tools/registry/contextual-tool-group.hpp
git commit -m "refactor: migrate ContextualToolGroup to deducing this"
```

---

### Task 6: Migrate QqMessageBuilder to deducing `this` (Layer 3)

**Files:**

- Modify: `src/channel/qq/qq-message-builder.hpp`
- Modify: `tests/channel/qq/qq-message-builder-test.cpp`

- [ ] **Step 1: Convert all chainable methods to deducing `this`**

Before (representative, `src/channel/qq/qq-message-builder.hpp` lines 13–21):

```cpp
QqMessageBuilder &text(std::string_view content) {
    payload_["content"] = content;
    payload_["msg_type"] = 0;
    payload_.erase("markdown");
    payload_.erase("media");
    payload_.erase("ark");
    payload_.erase("embed");
    return *this;
}
```

After:

```cpp
auto text(this auto &&self, std::string_view content) -> decltype(auto) {
    self.payload_["content"] = content;
    self.payload_["msg_type"] = 0;
    self.payload_.erase("markdown");
    self.payload_.erase("media");
    self.payload_.erase("ark");
    self.payload_.erase("embed");
    return std::forward<decltype(self)>(self);
}
```

Apply the same pattern to: `markdown()`, `media()`, `ark()`, `embed()`, `msg_seq()`, `reply_to()`, `reference()`, `keyboard()`.

- [ ] **Step 2: Fix redundant member initializer on `payload_` if clang-tidy flags it**

Before (`src/channel/qq/qq-message-builder.hpp` line 108):

```cpp
nlohmann::json payload_ = nlohmann::json::object();
```

Keep as-is only if `nlohmann::json` default-constructs to non-object. Since `nlohmann::json{}` default-constructs to `null`, the explicit `= nlohmann::json::object()` is required and not redundant.

- [ ] **Step 3: Update tests — remove `static_assert` type checks that relied on `QqMessageBuilder &` return types**

Before (`tests/channel/qq/qq-message-builder-test.cpp` lines 13–22):

```cpp
using TextSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
using MarkdownSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
using ReplySignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);
using ReferenceSignature = QqMessageBuilder &(QqMessageBuilder::*)(std::string_view);

static_assert(std::same_as<decltype(&QqMessageBuilder::text), TextSignature>);
static_assert(std::same_as<decltype(&QqMessageBuilder::markdown), MarkdownSignature>);
static_assert(std::same_as<decltype(&QqMessageBuilder::reply_to), ReplySignature>);
static_assert(std::same_as<decltype(&QqMessageBuilder::reference), ReferenceSignature>);
```

After: Remove these `static_assert` lines and the type aliases entirely. Deducing `this` methods are templates and cannot have their address taken with the old signatures. Replace with a compile-time chaining check:

```cpp
// Verify chainable API compiles with rvalue
static_assert(requires {
    { QqMessageBuilder{}.text("x").markdown("y").msg_seq(1).build() } -> std::same_as<nlohmann::json>;
});
```

- [ ] **Step 4: Build and test**

```bash
xmake build test-channel
ctest --test-dir build -R test-channel --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add src/channel/qq/qq-message-builder.hpp tests/channel/qq/qq-message-builder-test.cpp
git commit -m "refactor: migrate QqMessageBuilder to deducing this"
```

---

### Task 7: Migrate AutomationBuilder to deducing `this` + std::expected (Layer 4)

**Files:**

- Modify: `src/automation/builder.hpp`
- Modify: `src/automation/builder.cpp`
- Modify: `tests/automation/automation-model-test.cpp`

- [ ] **Step 1: Update `builder.hpp` — change chainable methods to deducing `this`, `build()` to `std::expected`**

Before (`src/automation/builder.hpp` lines 14–68, declaration):

```cpp
class AutomationBuilder {
public:
    explicit AutomationBuilder(std::string_view name);

    [[nodiscard]]
    AutomationBuilder &for_agent(std::string_view agent_key);

    // ... all other chainable methods return AutomationBuilder & ...

    [[nodiscard]]
    Automation build() const;

private:
    // ...
};
```

After:

```cpp
class AutomationBuilder {
public:
    explicit AutomationBuilder(std::string_view name);

    auto for_agent(this auto &&self, std::string_view agent_key) -> decltype(auto);
    auto run_prompt(this auto &&self, std::string_view prompt) -> decltype(auto);
    auto with_notes(this auto &&self, std::string_view notes) -> decltype(auto);
    auto cron(this auto &&self, std::string_view expression) -> decltype(auto);
    auto every(this auto &&self, std::chrono::seconds cadence) -> decltype(auto);
    auto jitter(this auto &&self, std::chrono::seconds amount) -> decltype(auto);
    auto once_at(this auto &&self, TimePoint scheduled_at) -> decltype(auto);
    auto time_zone(this auto &&self, std::string_view zone_name) -> decltype(auto);
    auto within_hours(this auto &&self, ActiveWindow window) -> decltype(auto);
    auto deliver_to(this auto &&self, std::string_view target) -> decltype(auto);
    auto deliver_silently(this auto &&self) -> decltype(auto);
    auto tag(this auto &&self, std::string_view value) -> decltype(auto);
    auto enable(this auto &&self) -> decltype(auto);
    auto disable(this auto &&self) -> decltype(auto);

    [[nodiscard]]
    auto build() const -> std::expected<Automation, std::string>;

private:
    [[nodiscard]]
    TriggerDefinition build_trigger() const;

    [[nodiscard]]
    std::optional<std::string> validate() const;

    // ... same member variables ...
};
```

Note: `validate()` changes from `void` (throwing) to `std::optional<std::string>` (returning first error). Add `#include <expected>` to the header.

- [ ] **Step 2: Update `builder.cpp` — convert all method implementations**

Since deducing `this` methods are templates, the chainable method bodies move into the header (inline). The `.cpp` file retains only `build()`, `build_trigger()`, and `validate()`.

For each chainable method, move to header inline. Example for `for_agent`:

Before (`src/automation/builder.cpp` lines 51–54):

```cpp
AutomationBuilder &AutomationBuilder::for_agent(std::string_view agent_key) {
    automation_.agent_key = std::string(agent_key);
    return *this;
}
```

After (in `builder.hpp`, inline):

```cpp
auto for_agent(this auto &&self, std::string_view agent_key) -> decltype(auto) {
    self.automation_.agent_key = std::string(agent_key);
    return std::forward<decltype(self)>(self);
}
```

- [ ] **Step 3: Convert `validate()` from throwing to returning errors**

Before (`src/automation/builder.cpp` lines 176–265):

```cpp
void AutomationBuilder::validate() const {
    validate_non_blank(automation_.name, "automation name");
    // ... many throw std::invalid_argument(...) ...
}
```

After:

```cpp
std::optional<std::string> AutomationBuilder::validate() const {
    if (is_blank(automation_.name)) { return "automation name must not be blank"; }
    if (is_blank(automation_.agent_key)) { return "agent key must not be blank"; }
    if (is_blank(automation_.prompt)) { return "prompt must not be blank"; }
    if (!trigger_kind_.has_value()) { return "trigger must be configured"; }
    // ... convert each throw to a return ...
    return std::nullopt;
}
```

- [ ] **Step 4: Convert `build()` to use `std::expected`**

Before (`src/automation/builder.cpp` lines 141–150):

```cpp
Automation AutomationBuilder::build() const {
    validate();
    auto automation = automation_;
    if (automation.id.empty()) {
        automation.id = generate_id("auto");
    }
    automation.trigger = build_trigger();
    return automation;
}
```

After:

```cpp
auto AutomationBuilder::build() const -> std::expected<Automation, std::string> {
    if (auto error = validate(); error.has_value()) {
        return std::unexpected(std::move(*error));
    }
    auto automation = automation_;
    if (automation.id.empty()) {
        automation.id = generate_id("auto");
    }
    automation.trigger = build_trigger();
    return automation;
}
```

- [ ] **Step 5: Fix clang-tidy modernization warnings in builder.cpp**

Replace `push_back` with `emplace_back` where applicable:

Before (`src/automation/builder.cpp` line 107):
```cpp
active_windows_.push_back(window);
```
After:
```cpp
active_windows_.emplace_back(window);
```

Before (`src/automation/builder.cpp` line 113):
```cpp
automation_.delivery.targets.push_back(std::string(target));
```
After:
```cpp
automation_.delivery.targets.emplace_back(target);
```

Before (`src/automation/builder.cpp` line 124):
```cpp
automation_.tags.push_back(std::string(value));
```
After:
```cpp
automation_.tags.emplace_back(value);
```

Replace manual loops with `std::ranges::all_of`/`any_of` in `validate()` where applicable:

Before:
```cpp
for (const auto &target : automation_.delivery.targets) {
    validate_non_blank(target, "delivery target");
}
```

After:
```cpp
if (!std::ranges::all_of(automation_.delivery.targets, [](std::string_view t) { return !is_blank(t); })) {
    return "delivery target must not be blank";
}
```

- [ ] **Step 6: Update ALL tests in `tests/automation/automation-model-test.cpp`**

Every test that calls `.build()` must handle `std::expected`. For success cases:

Before:
```cpp
const auto automation = orangutan::automation::Automation::named("repo-check")
                            .for_agent("default")
                            .run_prompt("scan repo and summarize changes")
                            .cron("0 9 * * *")
                            .build();
CHECK(automation.name == "repo-check");
```

After:
```cpp
const auto result = orangutan::automation::Automation::named("repo-check")
                        .for_agent("default")
                        .run_prompt("scan repo and summarize changes")
                        .cron("0 9 * * *")
                        .build();
REQUIRE(result.has_value());
const auto &automation = *result;
CHECK(automation.name == "repo-check");
```

For error cases, replace `CHECK_THROWS_AS(... .build(), std::invalid_argument)` with:

Before:
```cpp
CHECK_THROWS_AS(orangutan::automation::Automation::named("missing-agent")
                    .run_prompt("check")
                    .cron("0 9 * * *")
                    .build(),
                std::invalid_argument);
```

After:
```cpp
const auto result = orangutan::automation::Automation::named("missing-agent")
                        .run_prompt("check")
                        .cron("0 9 * * *")
                        .build();
CHECK_FALSE(result.has_value());
CHECK(result.error().contains("agent key"));
```

- [ ] **Step 7: Update any production callers of `AutomationBuilder::build()` in other source files**

Search for all callers:
```bash
rg "\.build\(\)" src/automation/ src/tools/automation/ --type cpp
```

Each caller that previously caught `std::invalid_argument` must now check `.has_value()`.

- [ ] **Step 8: Build and test**

```bash
xmake build test-automation
ctest --test-dir build -R test-automation --output-on-failure
```

- [ ] **Step 9: Commit**

```bash
git add src/automation/builder.hpp src/automation/builder.cpp tests/automation/automation-model-test.cpp
git commit -m "refactor: migrate AutomationBuilder to deducing this with std::expected"
```

---

### Task 8: Create AgentLoopBuilder (Layer 5)

**Files:**

- Modify: `src/agent/agent-loop.hpp`
- Modify: `tests/agent/agent-loop-test.cpp`

- [ ] **Step 1: Add `AgentLoopBuilder` class to `agent-loop.hpp`**

Add after the `AgentLoop` class, before the closing namespace:

```cpp
class AgentLoopBuilder {
public:
    AgentLoopBuilder(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools)
    : provider_(&provider),
      route_(std::move(route)),
      tools_(&tools) {}

    auto with_memory(this auto &&self, memory::RuntimeMemory *memory) -> decltype(auto) {
        self.memory_ = memory;
        return std::forward<decltype(self)>(self);
    }

    auto with_skills_prompt(this auto &&self, std::string prompt) -> decltype(auto) {
        self.skills_prompt_ = std::move(prompt);
        return std::forward<decltype(self)>(self);
    }

    auto with_hook_manager(this auto &&self, hooks::HookManager *manager) -> decltype(auto) {
        self.hook_manager_ = manager;
        return std::forward<decltype(self)>(self);
    }

    auto with_skill_loader(this auto &&self, skills::SkillLoader *loader) -> decltype(auto) {
        self.skill_loader_ = loader;
        return std::forward<decltype(self)>(self);
    }

    auto with_thinking_budget(this auto &&self, int budget) -> decltype(auto) {
        self.thinking_budget_ = budget;
        return std::forward<decltype(self)>(self);
    }

    auto with_environment_info(this auto &&self, prompt::EnvironmentInfo info) -> decltype(auto) {
        self.env_info_ = std::move(info);
        return std::forward<decltype(self)>(self);
    }

    auto with_incoming_message_fetcher(this auto &&self, AgentLoop::IncomingMessageFetcher fetcher) -> decltype(auto) {
        self.incoming_message_fetcher_ = std::move(fetcher);
        return std::forward<decltype(self)>(self);
    }

    auto with_stop_requested_callback(this auto &&self, AgentLoop::StopRequestedCallback callback) -> decltype(auto) {
        self.stop_requested_callback_ = std::move(callback);
        return std::forward<decltype(self)>(self);
    }

    auto with_history(this auto &&self, std::vector<Message> messages) -> decltype(auto) {
        self.history_ = std::move(messages);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    auto build() const -> std::expected<AgentLoop, std::string> {
        if (provider_ == nullptr) { return std::unexpected("provider is required"); }
        if (tools_ == nullptr) { return std::unexpected("tool registry is required"); }
        AgentLoop loop(*provider_, route_, *tools_, memory_, skills_prompt_, hook_manager_, skill_loader_);
        if (thinking_budget_ > 0) { loop.set_thinking_budget(thinking_budget_); }
        if (env_info_.has_value()) { loop.set_environment_info(*env_info_); }
        if (incoming_message_fetcher_) { loop.set_incoming_message_fetcher(incoming_message_fetcher_); }
        if (stop_requested_callback_) { loop.set_stop_requested_callback(stop_requested_callback_); }
        if (!history_.empty()) { loop.set_history(std::move(history_)); }
        return loop;
    }

private:
    ProviderSystem *provider_ = nullptr;
    ProviderRoute route_;
    ToolRegistry *tools_ = nullptr;
    memory::RuntimeMemory *memory_ = nullptr;
    std::string skills_prompt_;
    hooks::HookManager *hook_manager_ = nullptr;
    skills::SkillLoader *skill_loader_ = nullptr;
    int thinking_budget_ = 0;
    std::optional<prompt::EnvironmentInfo> env_info_;
    AgentLoop::IncomingMessageFetcher incoming_message_fetcher_;
    AgentLoop::StopRequestedCallback stop_requested_callback_;
    mutable std::vector<Message> history_;
};
```

Add `#include <expected>` to the header. Add a static factory on `AgentLoop`:

```cpp
[[nodiscard]]
static AgentLoopBuilder configure(ProviderSystem &provider, ProviderRoute route, ToolRegistry &tools) {
    return AgentLoopBuilder(provider, std::move(route), tools);
}
```

- [ ] **Step 2: Add tests for AgentLoopBuilder**

```cpp
TEST_CASE("agent_loop_builder_creates_loop_with_fluent_api") {
    ScriptedProvider provider({testing::make_text_response("hello")});
    ToolRegistry tools;

    auto result = AgentLoop::configure(provider.system, provider.route, tools)
                      .with_thinking_budget(512)
                      .build();

    REQUIRE(result.has_value());
    auto reply = result->run("hi");
    CHECK(reply == "hello");
}

TEST_CASE("agent_loop_builder_with_stop_callback") {
    ScriptedProvider provider({testing::make_text_response("unused")});
    ToolRegistry tools;

    auto result = AgentLoop::configure(provider.system, provider.route, tools)
                      .with_stop_requested_callback([] { return true; })
                      .build();

    REQUIRE(result.has_value());
    auto reply = result->run("stop now");
    CHECK(reply == "Task terminated.");
}
```

- [ ] **Step 3: Keep existing void setters on AgentLoop for backward compatibility**

The existing `set_thinking_budget()`, `set_environment_info()`, etc. remain unchanged. The builder is an additive API.

- [ ] **Step 4: Build and test**

```bash
xmake build test-agent
ctest --test-dir build -R test-agent --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add src/agent/agent-loop.hpp tests/agent/agent-loop-test.cpp
git commit -m "refactor: add AgentLoopBuilder with deducing this and std::expected"
```

---

### Task 9: Add chainable builder to SkillLoader (Layer 6)

**Files:**

- Modify: `src/skills/skill-loader.hpp`
- Modify: `src/skills/skill-loader.cpp`

- [ ] **Step 1: Add `SkillLoaderBuilder` class or `create()` factory to `SkillLoader`**

Add to `src/skills/skill-loader.hpp`:

```cpp
class SkillLoaderBuilder {
public:
    auto with_source(this auto &&self, skill_source source) -> decltype(auto) {
        self.source_ = source;
        return std::forward<decltype(self)>(self);
    }

    auto with_workspace_root(this auto &&self, std::filesystem::path root) -> decltype(auto) {
        self.workspace_root_ = std::move(root);
        return std::forward<decltype(self)>(self);
    }

    auto with_directories(this auto &&self, std::vector<std::filesystem::path> dirs) -> decltype(auto) {
        self.directories_ = std::move(dirs);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    auto build() const -> std::expected<SkillLoader, std::string> {
        if (workspace_root_.empty()) {
            return std::unexpected("workspace root is required");
        }
        SkillLoader loader;
        loader.set_source(source_);
        loader.set_workspace_root(workspace_root_);
        if (!directories_.empty()) {
            loader.load_from_directories(directories_);
        }
        return loader;
    }

private:
    skill_source source_ = skill_source::workspace;
    std::filesystem::path workspace_root_;
    std::vector<std::filesystem::path> directories_;
};
```

Add `#include <expected>` to the header. Add static factory:

```cpp
[[nodiscard]]
static SkillLoaderBuilder create() { return {}; }
```

- [ ] **Step 2: Keep existing void setters for backward compatibility**

The old `set_source()` and `set_workspace_root()` remain unchanged.

- [ ] **Step 3: Update bootstrap callers if desired (optional)**

Search for callers:
```bash
rg "SkillLoader" src/bootstrap/ --type cpp
```

Callers can migrate to the new builder API at their discretion.

- [ ] **Step 4: Build and test**

```bash
xmake build test-skills
ctest --test-dir build -R test-skills --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add src/skills/skill-loader.hpp src/skills/skill-loader.cpp
git commit -m "refactor: add SkillLoader chainable builder with std::expected"
```

---

### Task 10: Add chainable builder to WebServer (Layer 6)

**Files:**

- Modify: `src/web/web-server.hpp`
- Modify: `src/web/web-server.cpp`

- [ ] **Step 1: Add `WebServerBuilder` class to `web-server.hpp`**

```cpp
class WebServerBuilder {
public:
    auto with_static_dir(this auto &&self, std::filesystem::path path) -> decltype(auto) {
        self.static_dir_ = std::move(path);
        return std::forward<decltype(self)>(self);
    }

    auto with_session_store(this auto &&self, storage::SessionStore *store) -> decltype(auto) {
        self.session_store_ = store;
        return std::forward<decltype(self)>(self);
    }

    auto with_memory_store(this auto &&self, memory::MemoryStore *store) -> decltype(auto) {
        self.memory_store_ = store;
        return std::forward<decltype(self)>(self);
    }

    auto with_config(this auto &&self, config::Config *config) -> decltype(auto) {
        self.config_ = config;
        return std::forward<decltype(self)>(self);
    }

    auto with_config_save_path(this auto &&self, std::filesystem::path path) -> decltype(auto) {
        self.config_save_path_ = std::move(path);
        return std::forward<decltype(self)>(self);
    }

    auto with_tool_registry(this auto &&self, tools::ToolRegistry *registry) -> decltype(auto) {
        self.tool_registry_ = registry;
        return std::forward<decltype(self)>(self);
    }

    auto with_skill_loader(this auto &&self, skills::SkillLoader *loader) -> decltype(auto) {
        self.skill_loader_ = loader;
        return std::forward<decltype(self)>(self);
    }

    auto with_automation_service(this auto &&self, automation::AutomationService *service) -> decltype(auto) {
        self.automation_service_ = service;
        return std::forward<decltype(self)>(self);
    }

    auto with_automation_runtime(this auto &&self, automation::AutomationRuntime *runtime) -> decltype(auto) {
        self.automation_runtime_ = runtime;
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    auto build() -> std::expected<WebServer, std::string> {
        WebServer server;
        if (!static_dir_.empty()) { server.set_static_dir(static_dir_); }
        if (session_store_ != nullptr) { server.set_session_store(session_store_); }
        if (memory_store_ != nullptr) { server.set_memory_store(memory_store_); }
        if (config_ != nullptr) { server.set_config(config_); }
        if (!config_save_path_.empty()) { server.set_config_save_path(config_save_path_); }
        if (tool_registry_ != nullptr) { server.set_tool_registry(tool_registry_); }
        if (skill_loader_ != nullptr) { server.set_skill_loader(skill_loader_); }
        if (automation_service_ != nullptr) { server.set_automation_service(automation_service_); }
        if (automation_runtime_ != nullptr) { server.set_automation_runtime(automation_runtime_); }
        return std::move(server);
    }

private:
    std::filesystem::path static_dir_;
    storage::SessionStore *session_store_ = nullptr;
    memory::MemoryStore *memory_store_ = nullptr;
    config::Config *config_ = nullptr;
    std::filesystem::path config_save_path_;
    tools::ToolRegistry *tool_registry_ = nullptr;
    skills::SkillLoader *skill_loader_ = nullptr;
    automation::AutomationService *automation_service_ = nullptr;
    automation::AutomationRuntime *automation_runtime_ = nullptr;
};
```

Add `#include <expected>` to the header. Add static factory on `WebServer`:

```cpp
[[nodiscard]]
static WebServerBuilder create() { return {}; }
```

Note: `start()` and `stop()` remain non-chainable runtime methods on `WebServer`.

- [ ] **Step 2: Keep existing void setters for backward compatibility**

- [ ] **Step 3: Build and test**

```bash
xmake build test-web
ctest --test-dir build -R test-web --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add src/web/web-server.hpp src/web/web-server.cpp
git commit -m "refactor: add WebServer chainable builder with std::expected"
```

---

### Task 11: Add chainable builder to CoordinatorManager (Layer 6)

**Files:**

- Modify: `src/coordinator/coordinator-manager.hpp`
- Modify: `src/coordinator/coordinator-manager.cpp`

- [ ] **Step 1: Add `CoordinatorManagerBuilder` class**

```cpp
class CoordinatorManagerBuilder {
public:
    explicit CoordinatorManagerBuilder(int max_concurrent = 4)
    : max_concurrent_(max_concurrent) {}

    auto with_environment(this auto &&self, AgentExecutionEnvironment env) -> decltype(auto) {
        self.env_ = env;
        return std::forward<decltype(self)>(self);
    }

    auto with_notification_callback(this auto &&self, TaskNotificationCallback callback) -> decltype(auto) {
        self.notification_callback_ = std::move(callback);
        return std::forward<decltype(self)>(self);
    }

    auto with_worker_runtime_factory(this auto &&self, WorkerRuntimeFactory factory) -> decltype(auto) {
        self.worker_runtime_factory_ = std::move(factory);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]]
    auto build() -> std::expected<CoordinatorManager, std::string> {
        if (!worker_runtime_factory_) {
            return std::unexpected("worker runtime factory is required");
        }
        CoordinatorManager manager(max_concurrent_);
        manager.set_environment(env_);
        if (notification_callback_) { manager.set_notification_callback(std::move(notification_callback_)); }
        manager.set_worker_runtime_factory(std::move(worker_runtime_factory_));
        return std::move(manager);
    }

private:
    int max_concurrent_ = 4;
    AgentExecutionEnvironment env_;
    TaskNotificationCallback notification_callback_;
    WorkerRuntimeFactory worker_runtime_factory_;
};
```

Add `#include <expected>` to the header. Add static factory on `CoordinatorManager`:

```cpp
[[nodiscard]]
static CoordinatorManagerBuilder configure(int max_concurrent = 4) {
    return CoordinatorManagerBuilder(max_concurrent);
}
```

Note: Runtime methods (`spawn()`, `stop()`, `shutdown()`, etc.) stay as-is on `CoordinatorManager`.

- [ ] **Step 2: Keep existing void setters for backward compatibility**

- [ ] **Step 3: Build and test**

```bash
xmake build test-coordinator
ctest --test-dir build -R test-coordinator --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add src/coordinator/coordinator-manager.hpp src/coordinator/coordinator-manager.cpp
git commit -m "refactor: add CoordinatorManager chainable builder with std::expected"
```

---

### Task 12: Cross-module clang-tidy sweep

**Files:**

- All files modified in Tasks 1–11

- [ ] **Step 1: Replace `map.find(k) != map.end()` with `map.contains(k)` in all touched files**

Already done in Task 4 (`tool-dispatch.hpp`). Search for remaining instances:

```bash
rg "\.find\(" src/tools/registry/ src/automation/ src/coordinator/ --type cpp | rg "!= .+\.end()"
```

- [ ] **Step 2: Replace `std::sort(begin, end)` with `std::ranges::sort(v)` in touched files**

```bash
rg "std::sort\(" src/tools/ src/automation/ src/agent/ src/channel/ --type cpp
```

- [ ] **Step 3: Replace C-style casts with `static_cast<T>(x)` in touched files**

```bash
rg "\([a-z]+\)" src/tools/ src/automation/ src/agent/ src/channel/ --type cpp
```

- [ ] **Step 4: Add missing `explicit` on single-arg constructors in touched files**

Verify all single-argument constructors have `explicit`.

- [ ] **Step 5: Run clang-tidy on all modified files**

```bash
rg -l "" src/types/message.hpp src/types/tool-def.hpp src/tools/registry/ src/channel/qq/ src/automation/builder.* src/agent/agent-loop.hpp src/skills/skill-loader.hpp src/web/web-server.hpp src/coordinator/coordinator-manager.hpp | xargs clang-tidy -p build
```

- [ ] **Step 6: Commit**

```bash
git add -u
git commit -m "fix: cross-module clang-tidy modernization sweep"
```

---

### Task 13: Final integration verification

**Files:**

- All project files

- [ ] **Step 1: Build the entire project**

```bash
xmake build
```

- [ ] **Step 2: Run ALL tests**

```bash
xmake build -r test-types test-tools test-channel test-automation test-agent test-skills test-web test-coordinator test-bootstrap test-cli
ctest --test-dir build --output-on-failure
```

- [ ] **Step 3: Verify no regressions with broad grep for legacy patterns**

```bash
rg "return \*this" src/types/message.hpp src/tools/registry/tool-spec-builder.hpp src/tools/registry/tool-dispatch.hpp src/tools/registry/contextual-tool-group.hpp src/channel/qq/qq-message-builder.hpp src/automation/builder.hpp src/automation/builder.cpp
```

Expected: no matches in any of these files.

```bash
rg "throw std::invalid_argument" src/automation/builder.cpp
```

Expected: no matches (all converted to `std::unexpected`).

- [ ] **Step 4: Run full clang-tidy verification on all changed files**

```bash
git diff --name-only HEAD~13 HEAD -- '*.hpp' '*.cpp' | xargs clang-tidy -p build
```

- [ ] **Step 5: Final commit if any fixes were needed**

```bash
git add -u
git commit -m "fix: final integration verification fixes"
```
