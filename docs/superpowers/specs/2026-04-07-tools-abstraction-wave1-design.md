# 工具抽象层第一波重构设计（A 方案）

## 1. 背景与约束

本轮重构严格聚焦 `src/tools` 抽象层，目标是在不改变现有能力的前提下，清理重复、冗余和薄封装代码，并以 C++23 风格收敛实现。

硬约束：

- 工具名称保持不变（例如 `shell`、`read`、`agent_spawn`、`heartbeat` 等）。
- 输入参数 JSON 协议保持不变（字段名、类型、语义不变）。
- 输出结构与关键错误语义保持不变（含 `ToolRegistry::execute` 现有行为）。
- deferred / discoverability 语义保持不变。
- coordinator-only runtime 的工具注册与可见性行为保持不变。
- 采用可合并小步提交，允许逐步回滚。

### 1.1 本轮范围

本轮只处理 `src/tools`。`bootstrap/agent/channel/memory/utils` 的更深层重构不纳入第一波实现范围，避免把工具层抽象治理与 runtime 装配重构耦合到同一批变更中。

### 1.2 兼容策略

本轮采用严格兼容策略：

- 工具名不变。
- schema 字段与 required 集合不变。
- 错误文案保持现状。
- deferred 工具的 `definitions()/find_definition()/execute()` 行为保持现状。
- 仅重组内部注册与执行壳层，不对外引入协议演进。

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

### 2.6 memory 别名工具重复注册

`remember/memory_store`、`recall/memory_recall`、`forget/memory_forget` 存在同实现多名字的显式重复注册。兼容性要求决定这些别名必须继续保留，但当前实现方式重复度高且维护成本不必要地偏高。

## 3. 目标与非目标

### 3.1 目标

- 统一工具注册抽象层，减少重复样板。
- 统一上下文门禁表达方式，收敛“何时注册、何时执行”规则。
- 抽离可复用 schema 片段与 op 分发 helper。
- 为 automation 类 op 驱动工具建立统一执行壳层。
- 清理 coordinator/swarm 的薄封装注册层。
- 为 memory 工具建立轻量 alias 注册抽象。
- 保持 `ToolRegistry` 对外契约和行为不变。
- 通过现有测试回归验证功能等价。

### 3.2 非目标

- 不改工具名称、协议字段或输出 JSON 结构。
- 不改权限模型主流程（`runtime-loader` 中 `apply_permission_policy` 语义保持）。
- 不改 `shell/read/write/edit/memory/mcp/script` 的业务逻辑路径（第一波仅最小接入抽象层）。
- 不改 `ToolRegistry::execute` / output scrub / approval guard 主链路。
- 不做跨 `src/tools` 之外的 runtime 装配重构。
- 不引入与本次目标无关的新功能。

## 4. 设计方案（A：契约先行 + 渐进替换）

### 4.0 总体原则

- 只抽壳层，不抽业务。
- 公共 helper 不感知 `TaskSpec` / `HeartbeatSpec` / 团队生命周期等领域对象。
- 保持特例为特例：`message_attachments` 共享 op 壳层，但不强行并入 automation helper。
- 优先复用现有 builder / group / dispatch / schema 组件，而非再造一套全新的 DSL。

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

5. `op-tool-support.hpp`（新增，受限的 op 工具辅助层）
   - 默认不承接工具策略，只提供小粒度 helper：
     - `routed_input_with_default_op(...)`
     - `id is required` / `id or name is required` guard
     - `tool_dispatch().run(...).message` 的样板收敛
   - 不在第一波中统一缺省 `op` 策略。
   - 不统一错误 envelope（纯文本 vs JSON）。
   - 所有特化行为必须由工具显式声明，避免 helper 吞掉工具差异。

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

- `op-tool-support`：
  - 能按调用点显式给定的默认值进行 `op` 路由输入构造。
  - 能在不改变顶层返回类型的前提下收敛 `tool_dispatch().run(...).message` 样板。
  - 能复用既有 guard 文案而不改变错误文本。
  - 不能隐式决定某个工具缺省 `op` 的值或错误 envelope。

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

### 4.5 工具族分层

第一波按以下 4 类边界组织抽象，而不是追求单一“万能工具 DSL”:

- 基础 I/O 工具：`shell/file-read/file-edit/script/mcp`
- automation 类工具：`task/heartbeat/inbox/message_attachments`
- collaboration 类工具：`coordinator/*`、`swarm/*`
- compatibility/alias 类工具：`memory` 中同实现多名字工具

其中：

- automation 类工具共享 op 壳层，但保留各自领域逻辑。
- collaboration 类工具优先收敛注册层样板与 schema 重复。
- alias 类工具只统一注册方式，不改变外部可见名字集合。

## 5. 分阶段迁移计划（可合并小步提交）

### Commit 0：补齐兼容性基线测试

- 为 `register_builtin_tools` / `register_runtime_tools` 补齐工具可见性与执行语义基线：
  - deferred 工具在 `definitions()/find_definition()/execute()` 的现状行为
  - coordinator-only 立即 discover 且不注册 `tool_search`
  - `definition_filter` 对 `find_tool()/execute()` 的现有耦合
  - `message_attachments` 纯 JSON 错误 envelope
- 只补测试，不改实现。

### Commit 1：补足公共壳层（零行为变更）

- 新增 `op-tool-support`。
- 按需增强 `schema-fragments` 与 `contextual-tool-group`。
- 暂不迁移现有工具，只补充 helper 单测。

### Commit 2：automation 类 op 工具迁移

- 迁移：`task/heartbeat/inbox/message_attachments`。
- 统一 `op` 归一化、dispatch 外壳、常见 guard 文案。
- 保留现有上下文不可用提示与 `message_attachments` 的 channel 特例。

### Commit 3：coordinator + swarm 声明式迁移

- 迁移：`agent_spawn/agent_send_message/agent_stop`、`team_create/team_delete` 的注册样板。
- 保持 `register_coordinator_tools` 与 `register_swarm_tools` 签名不变。

### Commit 4：memory alias 收敛

- 为 `remember/memory_store`、`recall/memory_recall`、`forget/memory_forget` 建立轻量 alias 注册 helper。
- 保持所有别名工具继续存在且保持 deferred。

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

### 6.3 抽象规则

- registry helper 只负责 tool spec 组装、context gate、schema 片段、op 路由壳层。
- 业务模块继续负责参数语义校验、领域对象装配、runtime 调用和业务文案。
- 不建立跨业务的“超级自动化工具基类”。
- helper 不得快照会变化的 `ToolRuntimeContext` 状态；执行期仍必须读取 live `ToolRuntimeContext *`。

### 6.4 兼容性基线（Compatibility Oracle）

对第一波迁移工具建立基线检查矩阵（迁移前后输出必须一致）：

- 说明：第一波不再用 A/B 类泛化门禁，而使用逐工具矩阵，因为同一工具既可能在注册期 gate，也可能在执行期读取 live context 再次判定。

- 注册/执行矩阵：
  - `task`
    - 注册：`tool_context != nullptr && tool_context->automation_runtime != nullptr` 时注册。
    - 执行：若工具已注册，但执行时 `tool_context == nullptr || tool_context->automation_runtime == nullptr`，返回 `Error: task tool is not available in this context.`。
  - `heartbeat`
    - 注册：`tool_context != nullptr && tool_context->automation_runtime != nullptr` 时注册。
    - 执行：若工具已注册，但执行时 automation runtime 不可用，返回 `Error: heartbeat tool is not available in this context.`。
  - `inbox`
    - 注册：`tool_context != nullptr && tool_context->automation_runtime != nullptr` 时注册。
    - 执行：若工具已注册，但执行时 automation runtime 不可用，返回 `Error: inbox tool is not available in this context.`。
  - `message_attachments`
    - 注册：`tool_context != nullptr && runtime_origin == channel` 时注册。
    - 执行：继续读取 live `current_message_attachments` 与 `attachment_download_callback`，保持 JSON 错误 envelope。
  - `coordinator/swarm`
    - 注册：在 `register_builtin_tools(...)` 的上层条件满足后，由各自 registrars 注册；registrar 内部保持当前对 `tool_context` 的要求与 manager 缺失后的执行期错误语义。
    - 执行：manager 不可用、目标不存在等错误文本保持现状。

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

- `memory aliases`
  - `remember` 与 `memory_store` 同时存在。
  - `recall` 与 `memory_recall` 同时存在。
  - `forget` 与 `memory_forget` 同时存在。
  - 别名工具的 schema、deferred 可见性、执行行为保持兼容。

### 6.5 deferred / filter / execute 交互规则

- 规则 1：A 类工具（注册阶段门禁）在门禁失败时不注册，因此在 `find_definition()` 与 `execute()` 上均不可见。
- 规则 2：工具已注册且 `deferred=true` 时：
  - `definitions()` 默认不可见；
  - `find_definition()` 与 `find_tool()` 可查；
  - `execute()` 可直接执行（保持现状）。
- 规则 3：`discover_tool()` / `discover_deferred_tools()` 仅影响 `definitions()` 可见性，不影响执行能力。
- 规则 4：`definition_filter` 不仅影响 `definitions()`，还继续影响 `find_tool()`；因此执行 guard 中通过 `find_tool()` 读取工具元数据的行为保持现状。
- 规则 5：plan mode / deny rule 下被 filter 掉的工具，其执行 guard 行为与当前实现保持一致，不在第一波中解耦。

### 6.6 交互示例（deferred + gating + 兼容错误）

- 示例 1（A 类）：`message_attachments`
  - 条件：`tool_context == nullptr` 或 `runtime_origin != channel`
  - 结果：工具不注册；`find_definition("message_attachments") == nullptr`。
  - 说明：`{"error":"message_attachments tool is not available in this context."}` 不作为第一波兼容基线（该路径在门禁模型下不可达）。
- 示例 2（B 类）：`task`
  - 条件：`tool_context != nullptr` 但 `automation_runtime == nullptr`
  - 结果：在新的注册动作中该工具不会被注册；但对已注册工具执行时仍返回 `Error: task tool is not available in this context.`。

### 6.7 明确不变的核心执行语义

- deferred 工具默认隐藏于 `definitions()`，但可被 `find_definition()` 找到且可直接执行。
- coordinator-only 模式下继续立即 discover deferred tools，且不注册 `tool_search`。
- automation 类工具保留“注册期 gate + 执行期 unavailable 文本”双层兼容行为。
- `tool_dispatch::Response::is_error` 不在第一波中向 `ToolRegistry::execute` 的顶层 `is_error` 语义扩散。
- `definition_filter -> find_tool() -> execution_guard` 的现有耦合保持不变。
- 迁移后的工具定义必须满足 `ToolDef` 级别精确兼容，而非仅字段子集兼容。

### 6.8 ToolDef 精确兼容标准

对纳入迁移范围的工具，以下内容均视为兼容契约的一部分：

- `definition.name`
- `definition.description`
- `definition.input_schema` 的完整 JSON 结构
  - 包含 `type`
  - `properties`
  - `required`
  - `enum`
  - `minimum`
  - 以及任何现有附加字段

第一波的 schema/definition 抽象只能在生成结果与迁移前 `ToolDef` 完全等价时使用。

## 7. 测试与验收策略

### 7.1 回归优先顺序

1. `tests/tools/registry/tool-registry-test.cpp`
2. `tests/tools/coordinator/coordinator-tools-test.cpp`
3. `tests/tools/swarm/swarm-tools-test.cpp`
4. `tests/tools/task/task-tool-test.cpp`
5. `tests/tools/heartbeat/heartbeat-tool-test.cpp`
6. `tests/tools/inbox/inbox-tool-test.cpp`
7. `tests/tools/message-attachments/message-attachments-tool-test.cpp`
8. `tests/memory/memory-test.cpp`

### 7.2 重点验证点

- deferred 工具在 `definitions()`、`find_definition()`、`execute()` 的可见性行为一致。
- 工具协议兼容：输入字段缺失/错误类型时的错误语义一致。
- 上下文门禁：不可用上下文下注册或执行行为一致。
- coordinator/swarm 的团队消息与生命周期行为不回归。
- memory alias 工具的名字集合与行为兼容。
- `definition_filter` 影响 `find_tool()/execute()` 的行为不回归。
- 迁移后工具的 `ToolDef` 完整 JSON 与迁移前一致。

### 7.3 验收标准

- 受影响测试全部通过。
- 工具名称/参数/输出兼容性检查通过。
- 删除重复样板后满足量化阈值：
  - coordinator/swarm/automation-like/memory-alias 共 12 个工具注册点完成迁移或收敛；
  - 注册样板重复块（`registry.register_tool({.definition=...`）在目标文件中减少至少 35%；
  - 新增抽象层单测覆盖 5 个 helper（builder/group/dispatch/schema/op-support）。

## 8. 风险与回滚

主要风险：

- 抽象过度导致工具业务逻辑被错误泛化。
- 迁移中误改错误文案/字段，造成隐性兼容问题。
- 误把注册期 gate 与执行期 unavailable 语义合并，造成兼容性回归。
- 误把 `tool_dispatch::Response::is_error` 上推到顶层结果语义，改变测试预期。

缓解策略：

- 每次仅迁移一个工具族并立刻跑对应测试。
- 保留原行为快照测试（尤其错误分支）。
- 小步提交，出现回归时可按 commit 级别快速回退。
- 对 coordinator-only runtime 与 deferred 可见性增加回归验证。

## 9. C++23 落地约定

- helper API 参数优先 `std::string_view` / `std::span`。
- helper 中优先使用 `std::string_view`、局部 lambda、范围算法和小型声明式组合，避免引入大型继承层次。
- 若 `std::expected` 能在不扩大改动面的前提下简化局部分支，可在 helper 内部采用；不强制把所有既有工具改写为 `expected` 风格。
- 统一使用范围算法与结构化初始化，避免重复样板。
- 保持现有命名与 clang-tidy 约束一致。

## 10. 第一波完成定义（Definition of Done）

- `src/tools` 抽象层完成第一轮统一，`coordinator/swarm/automation-like/memory-alias` 目标完成迁移或收敛。
- 所有既有协议与关键行为保持不变。
- 兼容性基线矩阵全部通过，且无新增已知功能回归。
- 为第二波（`shell/read/write/edit/memory/mcp/script` 深层收敛）留下稳定抽象基础。
