# 工具抽象层第一波重构设计（A 方案）

## 1. 背景与约束

本轮重构聚焦 `src/tools` 抽象层，目标是在不改变现有能力的前提下，清理重复、冗余和薄封装代码，并以 C++23 风格收敛实现。

硬约束：

- 工具名称保持不变（例如 `shell`、`read`、`agent_spawn`、`heartbeat` 等）。
- 输入参数 JSON 协议保持不变（字段名、类型、语义不变）。
- 输出结构与关键错误语义保持不变（含 `ToolRegistry::execute` 现有行为）。
- 采用可合并小步提交，允许逐步回滚。

## 2. 现状扫描与问题定位

### 2.1 重复注册样板

在多个工具模块中，存在重复的 `registry.register_tool({...})` 模板化代码，包含重复字段拼装（`definition`、`input_schema`、`execute`、`deferred`）。

典型位置：

- `src/tools/coordinator/agent-spawn-tool.cpp`
- `src/tools/coordinator/agent-send-message-tool.cpp`
- `src/tools/coordinator/agent-stop-tool.cpp`
- `src/tools/swarm/team-create-tool.cpp`
- `src/tools/swarm/team-delete-tool.cpp`
- `src/tools/task/task-tool.cpp`
- `src/tools/heartbeat/heartbeat-tool.cpp`
- `src/tools/inbox/inbox-tool.cpp`
- `src/tools/message-attachments/message-attachments-tool.cpp`

### 2.2 上下文门禁与可用性检查分散

多处重复逻辑：

- 注册阶段检查 `tool_context == nullptr` 或 `tool_context->automation_runtime == nullptr`。
- 执行阶段再检查一次“当前上下文不可用”。

这一模式在 `task/heartbeat/inbox/message_attachments` 等工具中重复出现，导致行为边界分散且难以统一校验。

### 2.3 op 分发与错误文案样式重复

`task`、`heartbeat`、`inbox` 都实现了类似的：

- `op` 读取与合法性判断
- `id/name` 参数门禁
- 分支执行（`list/add/update/remove/run/...`）
- 统一错误前缀字符串拼装

实现重复但分散，维护成本高。

### 2.4 薄中转层增多

`register.hpp/register.cpp` 与单工具注册函数之间存在机械中转层，部分文件只承担“收集函数数组并转发调用”的职责，抽象收益低。

### 2.5 schema 片段重复

大量 `input_schema = {{"type", "object"}, ...}` 片段重复出现，尤其是 `op/id/name`、`delivery_mode/targets`、空对象 schema 等。

## 3. 目标与非目标

### 3.1 目标

- 统一工具注册抽象层，减少重复样板。
- 统一上下文门禁表达方式，收敛“何时注册、何时执行”规则。
- 抽离可复用 schema 片段与 op 分发 helper。
- 保持 `ToolRegistry` 对外契约和行为不变。
- 通过现有测试回归验证功能等价。

### 3.2 非目标

- 不改工具名称、协议字段或输出 JSON 结构。
- 不改权限模型主流程（`runtime-loader` 中 `apply_permission_policy` 语义保持）。
- 不改 `shell/read/write/edit/memory/mcp/script` 的业务逻辑路径（第一波仅最小接入抽象层）。
- 不引入与本次目标无关的新功能。

## 4. 设计方案（A：契约先行 + 渐进替换）

### 4.1 新增内部抽象（不改变外部 API）

在 `src/tools/registry/` 新增如下固定组件：

1. `tool-spec-builder.hpp`（声明式构建 `Tool`）
   - 输入：`ToolDef`、`read_only`、`deferred`、`check_permissions`、`execute/execute_rich`。
   - 输出：完整 `Tool` 实例。
   - 不变量：
     - 至少存在一个执行入口（`execute` 或 `execute_rich`）。
     - 不修改传入 `ToolDef` 的字段语义。

   配套 `ToolSpec`（仅内部使用）最小契约：

   - 字段：`ToolDef definition`、`bool read_only`、`bool deferred`、
     `std::function<PermissionResult(const ToolUse&, const ToolPermissionContext&)> check_permissions`、
     `std::function<std::string(const nlohmann::json&)> execute`、
     `std::function<ToolOutput(const nlohmann::json&)> execute_rich`。
   - 约束：`execute` 与 `execute_rich` 二选一或同时存在。
   - 边界：`ToolSpec` -> `Tool` 转换只在 builder 中发生，业务模块只声明 `ToolSpec`。

2. `contextual-tool-group.hpp`（上下文门禁 + 批量注册）
   - 提供门禁枚举：
     - `require_tool_context`
     - `require_automation_runtime`
     - `require_channel_origin`
   - 语义：
     - `require_tool_context`：`tool_context != nullptr`
     - `require_automation_runtime`：`tool_context != nullptr && tool_context->automation_runtime != nullptr`
     - `require_channel_origin`：`tool_context != nullptr && tool_context->runtime_origin == base::origin::channel`
   - 行为：门禁不满足时“不注册该工具”，不抛异常。

3. `tool-dispatch.hpp`（op 路由 helper）
   - 输入：`op` 字段、路由表、默认错误生成器。
   - 输出：匹配分支执行结果或错误文本。
   - 约束：工具可注入自定义错误文案，保证兼容现有输出文本。

4. `schema-fragments.hpp`（可复用 JSON schema 片段）
   - 提供空对象、`op` 枚举、`id` 字段、delivery 字段片段。
   - 仅减少重复，不改字段名、类型、必填策略。

### 4.2 接口契约与独立可测单元

每个新增组件必须可独立测试：

- `tool-spec-builder`：
  - 能构建与手写初始化列表等价的 `Tool`。
  - `deferred/read_only/check_permissions` 保持值一致。
- `contextual-tool-group`：
  - 给定不同 `ToolRuntimeContext` 时，注册集合可预测。
  - 门禁失败仅跳过注册，不影响其他工具。
- `tool-dispatch`：
  - 已知 `op` 路径命中正确 handler。
  - 未知 `op` 与缺字段路径返回预期错误文本。
- `schema-fragments`：
  - 组合后 schema 与迁移前 schema 在关键字段上等价。

### 4.3 与现有核心执行链的关系

以下链路保持不变：

- `register_runtime_tools(...)` 调用时机与流程保持。
- `ToolRegistry::execute(...)` 的查找、异常包装、输出 scrub 保持。
- deferred discovery（`discover_tool/discover_deferred_tools/tool_search`）行为保持。

新增抽象仅作用于“注册和内部组织方式”，不改变执行主干。

### 4.4 抽象边界

- 业务行为留在原工具模块。
- 抽象层只负责：注册声明、门禁表达、通用路由。
- 避免把具体业务逻辑（如调度、团队生命周期）推入抽象层。

## 5. 分阶段迁移计划（可合并小步提交）

### Commit 1：抽象骨架落地（零行为变更）

- 新增 `tool-spec-builder/contextual-tool-group/tool-dispatch/schema-fragments`。
- 暂不迁移现有工具，只补充 helper 单测。

### Commit 2：coordinator + swarm 声明式迁移

- 迁移：`agent_spawn/agent_send_message/agent_stop`、`team_create/team_delete` 的注册样板。
- 保持 `register_coordinator_tools` 与 `register_swarm_tools` 签名不变。

### Commit 3：automation 风格工具迁移

- 迁移：`task/heartbeat/inbox/message_attachments`。
- 引入统一上下文门禁与 op 分发 helper。
- 严格保持字段与文案兼容。

### Commit 4：schema 与重复校验逻辑收敛

- 将高频 schema 片段替换为公共 helper。
- 收敛重复的“上下文不可用”判定路径，保持工具输出兼容。

### Commit 5：删除冗余薄封装与清理

- 清理不再需要的中转层与重复 helper。
- 保证 `runtime-loader` 外观与行为不变。

## 6. 数据流与错误处理统一规则

### 6.1 注册数据流

1. bootstrap/runtime 调用 `register_runtime_tools`。
2. 进入 builtin/custom/mcp 注册阶段。
3. 新抽象负责将声明式 `ToolSpec` 转换为 `Tool` 并注册。
4. `ToolRegistry` 按既有逻辑管理 deferred 与 filter。

### 6.2 执行与错误规则

- 工具执行错误仍通过异常或显式错误字符串上送。
- `ToolRegistry::execute` 继续统一转为 `ToolResult{is_error}`。
- 输出 scrub 逻辑保持，敏感信息规避不变。
- 未知工具、权限拒绝、审批拒绝等行为路径保持。

### 6.3 兼容性基线（Compatibility Oracle）

对第一波迁移工具建立基线检查矩阵（迁移前后输出必须一致）：

- 说明：第一波的上下文策略分两类，且保持现状不变。
  - A 类（注册阶段门禁）：`coordinator/swarm/message_attachments`，门禁失败时不注册。
  - B 类（执行阶段提示）：`task/heartbeat/inbox`，可能注册但执行时返回“not available in this context”。

- `task`
  - `op` 非法：`Error: unknown operation. Supported: add, update, remove, list, run.`
  - 缺少 `id/name`（remove/run/update）：`Error: id or name is required.`
  - 缺少上下文：`Error: task tool is not available in this context.`
- `heartbeat`
  - `op` 非法：`Error: unknown operation. Supported: add, update, remove, list, run, pause, resume.`
  - 缺少 `id/name`（remove/run/pause/resume/update）：`Error: id or name is required.`
  - 缺少上下文：`Error: heartbeat tool is not available in this context.`
- `inbox`
  - `op` 非法：`Error: unknown operation. Supported: list, ack, clear.`
  - `ack` 缺少 `id`：`Error: id is required.`
  - 缺少上下文：`Error: inbox tool is not available in this context.`
- `message_attachments`
  - 注册门禁失败（非 channel 或无 `tool_context`）：工具不注册。
  - 已注册但当前消息无附件：`{"error":"the current message has no attachments."}`
  - 非法 `op`：`{"error":"unknown operation. Supported: list, download."}`
  - `download` 缺少回调：`{"error":"attachment downloads are not available in this context."}`
- `coordinator/swarm`
  - 工具定义仍为 deferred。
  - `find_definition("agent_spawn"/"team_create")` 可见性不变。
  - 关键失败路径（manager 不可用、目标不存在）输出保持现状。

### 6.4 deferred 与门禁交互规则

- 规则 1：A 类工具（注册阶段门禁）在门禁失败时不注册，因此在 `find_definition()` 与 `execute()` 上均不可见。
- 规则 2：B 类工具（执行阶段提示）允许注册；执行时返回既有上下文错误文本（用于保持兼容）。
- 规则 3：工具已注册且 `deferred=true` 时：
  - `definitions()` 默认不可见；
  - `find_definition()` 与 `find_tool()` 可查；
  - `execute()` 可直接执行（保持现状）。
- 规则 4：`discover_tool()` / `discover_deferred_tools()` 仅影响 `definitions()` 可见性，不影响执行能力。
- 规则 5：`definition_filter`（权限/plan mode）继续在定义展示层生效，不改变 `ToolRegistry::execute` 入口契约。

### 6.5 交互示例（deferred + gating + 兼容错误）

- 示例 1（A 类）：`message_attachments`
  - 条件：`tool_context == nullptr` 或 `runtime_origin != channel`
  - 结果：工具不注册；`find_definition("message_attachments") == nullptr`。
  - 说明：`{"error":"message_attachments tool is not available in this context."}` 不作为第一波兼容基线（该路径在门禁模型下不可达）。
- 示例 2（B 类）：`task`
  - 条件：`tool_context != nullptr` 但 `automation_runtime == nullptr`
  - 结果：工具可见；执行返回 `Error: task tool is not available in this context.`。

## 7. 测试与验收策略

### 7.1 回归优先顺序

1. `tests/tools/registry/tool-registry-test.cpp`
2. `tests/tools/coordinator/coordinator-tools-test.cpp`
3. `tests/tools/swarm/swarm-tools-test.cpp`
4. `tests/tools/task/task-tool-test.cpp`
5. `tests/tools/heartbeat/heartbeat-tool-test.cpp`
6. `tests/tools/inbox/inbox-tool-test.cpp`
7. `tests/tools/message-attachments/message-attachments-tool-test.cpp`

### 7.2 重点验证点

- deferred 工具在 `definitions()`、`find_definition()`、`execute()` 的可见性行为一致。
- 工具协议兼容：输入字段缺失/错误类型时的错误语义一致。
- 上下文门禁：不可用上下文下注册或执行行为一致。
- coordinator/swarm 的团队消息与生命周期行为不回归。

### 7.3 验收标准

- 受影响测试全部通过。
- 工具名称/参数/输出兼容性检查通过。
- 删除重复样板后满足量化阈值：
  - coordinator/swarm/automation-like 共 9 个工具注册点完成迁移；
  - 注册样板重复块（`registry.register_tool({.definition=...`）在目标文件中减少至少 35%；
  - 新增抽象层单测覆盖 4 个 helper（builder/group/dispatch/schema）。

## 8. 风险与回滚

主要风险：

- 抽象过度导致工具业务逻辑被错误泛化。
- 迁移中误改错误文案/字段，造成隐性兼容问题。

缓解策略：

- 每次仅迁移一个工具族并立刻跑对应测试。
- 保留原行为快照测试（尤其错误分支）。
- 小步提交，出现回归时可按 commit 级别快速回退。

## 9. C++23 落地约定

- helper API 参数优先 `std::string_view` / `std::span`。
- 尽量使用 `std::expected` 组织解析错误分支，减少嵌套 if。
- 统一使用范围算法与结构化初始化，避免重复样板。
- 保持现有命名与 clang-tidy 约束一致。

## 10. 第一波完成定义（Definition of Done）

- `src/tools` 抽象层完成第一轮统一，`coordinator/swarm/automation-like` 工具迁移完成。
- 所有既有协议与关键行为保持不变。
- 兼容性基线矩阵全部通过，且无新增已知功能回归。
- 为第二波（`shell/read/write/edit/memory/mcp/script` 深层收敛）留下稳定抽象基础。
