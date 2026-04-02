# Orangutan vs Claude Code: 深度对比分析与改进报告

## 一、架构总览对比

| 维度 | Claude Code | Orangutan |
|------|------------|-----------|
| 语言 | TypeScript (Bun) | C++23 (xmake) |
| 源文件数 | ~1332 .ts | ~194 .cpp/.hpp |
| 核心循环 | query.ts (1729行) + QueryEngine.ts (1296行) | agent-loop.cpp (642行) |
| 工具数 | 40+ 独立工具目录 | 16 工具目录 |
| 系统提示词 | ~900行动态生成 (prompts.ts) | ~10行硬编码默认值 |
| 会话持久化 | 文件级transcript + 结构化恢复 | SQLite SessionStore |
| 压缩机制 | 多层：auto-compact, micro-compact, snip-compact, FRC | 单层：LLM摘要压缩 |
| 权限系统 | 多模式(plan/acceptEdits/bypassPermissions/auto) + 细粒度规则 | 基础allow/deny/ask三态 |
| MCP支持 | 完整(auth, resources, instructions, delta) | 基础(connect/tools) |
| 子代理 | 完整团队系统(Team/Task/SendMessage) | spawn/status/wait三工具 |
| 记忆系统 | 文件目录(memdir) + MEMORY.md索引 + 自动注入 | SQLite存储 + 向量化搜索 |
| Hooks | settings.json配置 + 结构化事件 | 目录文件加载 + 6类事件 |

---

## 二、提示词工程：核心差距

这是**最关键的差距**。Claude Code 的系统提示词是一个精心工程化的、约 900 行的动态生成系统。Orangutan 使用 ~10 行硬编码默认值。

### 2.1 Claude Code 的系统提示词架构

Claude Code 将系统提示词分为**静态段**和**动态段**，中间以 `SYSTEM_PROMPT_DYNAMIC_BOUNDARY` 分隔，用于 API 级别的 prompt caching：

```
[静态段 - 跨组织可缓存]
├── 角色介绍 (getSimpleIntroSection)
├── 系统行为 (getSimpleSystemSection)  
├── 任务指南 (getSimpleDoingTasksSection)
├── 安全行动 (getActionsSection)
├── 工具使用 (getUsingYourToolsSection)
├── 语气风格 (getSimpleToneAndStyleSection)
├── 输出效率 (getOutputEfficiencySection)
├── === BOUNDARY ===
[动态段 - 会话特定]
├── 会话指导 (session-specific guidance)
├── 记忆提示 (auto memory)
├── 环境信息 (env info: cwd, git, model, os)
├── 语言偏好
├── 输出风格
├── MCP指令
├── 临时目录
└── 函数结果清理
```

### 2.2 Orangutan 当前的问题

```cpp
// agent-loop.cpp:23-25
constexpr std::string_view default_system_prompt = 
    "You are Orangutan, a helpful AI assistant that can use tools to help the user.\n"
    "You run on the user's local machine.\n"
    "When you need to run commands or read files, use the provided tools.\n"
    "Be concise and helpful.";
```

**问题清单：**

1. **没有安全指导** — Claude Code 有完整的 OWASP 安全意识、可逆性分析、blast radius 评估
2. **没有代码风格指导** — 不写多余注释、不引入不必要的抽象、不过度工程化
3. **没有工具使用指导** — 什么时候用文件工具而不是shell、什么时候并行调用
4. **没有输出控制** — 简洁性、格式规范、代码引用格式
5. **没有环境感知** — git状态、分支、OS信息、当前模型能力
6. **没有prompt caching优化** — 静态/动态分离
7. **没有记忆注入** — 虽然有记忆系统但注入方式简陋

### 2.3 实施方案：提示词工程化重构

**文件位置：** 新建 `src/prompt/system-prompt-builder.hpp` 和 `.cpp`

```
src/prompt/
├── system-prompt-builder.hpp    // 主构建器
├── system-prompt-builder.cpp
├── sections/
│   ├── identity.cpp           // 角色定义
│   ├── system-behavior.cpp    // 系统行为规范
│   ├── task-guidelines.cpp    // 任务执行指南
│   ├── safety-actions.cpp     // 安全行动指南
│   ├── tool-usage.cpp         // 工具使用规范
│   ├── tone-style.cpp         // 语气和风格
│   ├── output-efficiency.cpp  // 输出效率
│   ├── environment.cpp        // 环境信息(git/os/model)
│   └── memory-injection.cpp   // 记忆注入
└── prompt-cache.hpp           // 缓存边界管理
```

**关键实现点：**

1. **静态/动态分离**：静态段跨会话不变，动态段包含会话特定信息。API 请求时静态段可以利用 Anthropic 的 prompt caching。
2. **环境感知注入**：自动检测 git 仓库、分支、语言运行时环境并注入到系统提示中。
3. **工具引导生成**：根据实际注册的工具列表动态生成使用指南，告诉模型优先使用文件工具而非 shell。
4. **安全/质量规范**：从 Claude Code 的 prompts.ts 中移植关键安全和代码质量指南。

**对接方式：** 修改 `AgentLoop::build_system_prompt` 从调用 `SystemPromptBuilder` 实例获取完整提示词，替换当前的字符串拼接。

---

## 三、历史压缩：从单层到多层

### 3.1 Claude Code 的多层压缩

Claude Code 有 **5 种** 压缩机制协同工作：

| 机制 | 触发条件 | 效果 |
|------|---------|------|
| auto-compact | token估算接近上下文限制 | 全量历史压缩 |
| micro-compact | 工具结果过大 | 清理旧的工具结果内容 |
| api-microcompact | API级别 | 服务端透明压缩 |
| snip-compact | 标记点 | 截断到标记点 |
| FRC (Function Result Clearing) | 最近N条之外 | 自动清理旧函数结果 |

### 3.2 Orangutan 当前的问题

```cpp
// agent-loop.cpp:405-466
// 只有一种方式：让LLM摘要前面的消息
AgentLoop::HistoryCompactionResult AgentLoop::compact_history(std::size_t minimum_history_size)
```

**问题：**
- 仅 LLM 摘要一种方式，无分层机制
- 没有 token 估算，用 `compaction_threshold=50` 消息数代替
- 工具结果不压缩 — 大文件读取结果永远占据上下文
- 没有自动触发，需要外部调用或手动 `/compact`
- 压缩后没有清理措施

### 3.3 实施方案

**阶段一：Token 估算 + 自动触发**
- 新建 `src/context/token-estimator.hpp`
- 实现基于字符数的粗估 + 基于 tiktoken 的精估（可选）
- 在 `AgentLoop::run` 循环中每次迭代前检测，而不是等消息数到50

**阶段二：工具结果微压缩**
- 新建 `src/context/tool-result-compactor.hpp`
- 保留最近 N 条工具结果完整内容
- 旧工具结果替换为 `[Tool result from {tool_name}: {first_100_chars}... (cleared)]`
- 在每次 `execute_tools` 返回后执行

**阶段三：增量压缩**
- 不每次全量摘要，而是按段压缩
- 保留最近 10 条完整消息（当前设计）
- 被压缩的段生成边界标记，后续可以恢复上下文

**对接方式：** 
- `AgentLoop::run` 循环开头加 token 检测
- `execute_tools` 之后加工具结果微压缩
- `compact_history` 重构为多层策略模式

---

## 四、权限系统：从三态到细粒度

### 4.1 Claude Code 的权限系统

Claude Code 有多个权限维度：

1. **Permission Mode**: plan, acceptEdits, bypassPermissions, default, auto, dontAsk
2. **Tool-level permissions**: 每个工具独立的允许/拒绝规则
3. **File-level permissions**: 读/写/创建 不同文件路径的权限
4. **Command semantics**: 命令语义分析 (destructiveCommandWarning.ts, readOnlyValidation.ts)
5. **Auto-allow rules**: 基于模式匹配的自动允许
6. **Yolo classifier**: 基于上下文自动判断是否需要确认

### 4.2 Orangutan 的现状

```cpp
enum class ToolApprovalPolicy { ask, allow, deny };
struct ToolPermissionSettings {
    ToolSandboxMode sandbox_mode;
    ToolApprovalPolicy shell_approval;
    std::vector<std::string> allowed_tools;
    std::vector<std::string> denied_tools;
    std::vector<std::string> denied_shell_commands;
};
```

**缺失：**
- 没有 permission mode 概念 — 无法全局切换"计划模式"/"自动模式"
- 没有文件级权限 — 无法区分读/写/创建
- 没有命令语义分析 — `rm -rf /` 和 `ls` 同等对待
- 没有自动允许模式匹配 — 比如允许所有 `git status` 类命令
- 没有 plan mode — 这是 Claude Code 最重要的交互模式之一

### 4.3 实施方案

**阶段一：Permission Mode**
```cpp
enum class PermissionMode {
    default_mode,     // 每次确认
    plan,            // 仅计划需要确认
    auto_mode,       // 智能判断
    accept_edits,    // 文件编辑自动允许
    bypass,          // 全部允许
};
```
- 新建 `src/tools/registry/permission-mode.hpp`
- 在 ToolRegistry::execute 时根据 mode 决定是否需要确认

**阶段二：命令风险分析**
```cpp
enum class CommandRisk { safe, moderate, dangerous, destructive };
CommandRisk analyze_command_risk(std::string_view command);
```
- `rm -rf`, `git push --force`, `DROP TABLE` 等标记为 destructive
- `git status`, `ls`, `cat` 标记为 safe
- 在 shell 工具执行前自动分类

**阶段三：Plan Mode**
- 这是一个核心交互模式：agent 先探索和规划，然后提交计划让用户审批
- 需要新建 `src/plan/plan-mode.hpp`
- plan 文件存储在 `.orangutan/plans/`
- 与 AgentLoop 集成：plan mode 下只允许读取工具，不允许写入

---

## 五、工具生态差距

### 5.1 缺失的重要工具

| Claude Code 工具 | 用途 | Orangutan 等价物 | 优先级 |
|-----------------|------|-----------------|--------|
| GlobTool | 文件模式搜索 | 无（依赖shell） | **高** |
| GrepTool | 代码内容搜索 | 无（依赖shell） | **高** |
| AskUserQuestion | 结构化用户交互 | 无 | **高** |
| LSPTool | 代码智能(定义跳转等) | 无 | 中 |
| WebFetchTool | URL内容抓取 | 无 | 中 |
| WebSearchTool | 网络搜索 | 无 | 中 |
| EnterPlanModeTool | 计划模式 | 无 | **高** |
| NotebookEditTool | Jupyter支持 | 无 | 低 |
| ScheduleCronTool | 定时任务 | 有(automation) | — |
| TaskCreate/Update/List/Get | 任务管理 | 有(task-tool) | — |
| TeamCreate/Delete | 团队协作 | 无 | 中 |
| SendMessageTool | 代理间通信 | 无(inbox有类似) | 中 |
| SkillTool | 技能调用 | 有(skill-loader) | — |

### 5.2 GlobTool 实施方案

**最高优先级** — 当前模型只能通过 shell 的 `find` 来搜索文件，这导致：
1. 提示词无法指导模型"用Glob而不是find"
2. shell 调用开销大（fork+exec）
3. 没有权限隔离

```
src/tools/glob/
├── glob-tool.cpp
├── register.cpp
└── register.hpp
```

实现要点：
- 使用 `std::filesystem::recursive_directory_iterator`
- 支持 `**/*.cpp`, `src/**/*.hpp` 等 glob 模式
- 按修改时间排序返回
- 限制最大返回数量（默认250）
- 对接：在 `register_builtin_tools` 中注册

### 5.3 GrepTool 实施方案

```
src/tools/grep/
├── grep-tool.cpp
├── register.cpp
└── register.hpp
```

实现要点：
- 使用 `std::regex` 或 PCRE2 进行内容搜索
- 支持 output_mode: files_with_matches, content, count
- 支持 -A/-B/-C 上下文行
- 支持文件类型过滤
- 性能关键：需要并发文件读取
- 对接：在 `register_builtin_tools` 中注册

### 5.4 AskUserQuestion 工具

```
src/tools/ask-user/
├── ask-user-tool.cpp
├── register.cpp
└── register.hpp
```

实现要点：
- 支持多选/单选问题
- 支持 2-4 个选项
- CLI模式通过stdin读取
- Web模式通过WebSocket推送
- Channel模式通过消息回传
- 这不只是一个工具，还需要 AgentLoop 暂停等待用户输入

---

## 六、会话管理与恢复

### 6.1 Claude Code 的会话系统

- **Transcript 持久化**：每条消息实时写入文件
- **结构化恢复**：compact boundary + preserved segment + relink
- **Session ID**：全局唯一，可跨进程恢复
- **File History**：每个用户消息前对工作目录做快照
- **Eager Flush**：确保 desktop 杀进程时不丢数据

### 6.2 Orangutan 的现状

- SQLite 存储，有 save/load/append
- JID 绑定（channel 模式）
- 基本的 resume 功能

**缺失：**
- 没有实时持久化（只在会话结束或手动保存时写入）
- 没有 file history/snapshot
- compact 后没有结构化边界标记
- channel模式下没有auto-save间隔

### 6.3 实施方案

**阶段一：增量持久化**
- `AgentLoop::run` 的 `on_history_checkpoint` 回调中实现增量 append
- 每次工具执行后自动 append 新消息到 session store
- 不需要等到整个 run() 结束

**阶段二：Compact Boundary**
- compact 后插入边界标记消息
- resume 时从边界标记后开始
- 这样可以避免重新压缩已经压缩过的内容

---

## 七、API 层优化

### 7.1 Claude Code 的 API 策略

- **Prompt Caching**：静态/动态分离 + Blake2b hash 前缀匹配
- **Retry with backoff**：429/5xx 指数退避
- **Usage tracking**：精确 token 计数 + 成本计算
- **Fast mode**：同模型快速输出模式
- **Thinking config**：adaptive/enabled/disabled 三态

### 7.2 Orangutan 的缺失

- 没有 prompt caching 感知
- Provider 有 fallback 但 retry 策略不详
- 没有 cost tracking（Config 有 ModelCostConfig 但未实现）
- thinking_budget 是简单整数，不是 adaptive 模式

### 7.3 实施方案

**Cost Tracking**：
- 新建 `src/providers/cost-tracker.hpp`
- 从 LLMResponse 中提取 usage 字段
- 根据 ModelCostConfig 计算费用
- 在 CLI/Web 中展示当前会话成本
- 支持 max_budget_usd 限制

**Prompt Cache 优化**：
- 将系统提示词分为 cache_control 标记的段
- 使用 Anthropic API 的 `cache_control: {"type": "ephemeral"}` 特性
- 对接：在 Provider::chat_stream 构建请求时注入 cache_control

---

## 八、子代理与团队系统

### 8.1 Claude Code 的团队系统

Claude Code 有一个完整的多代理协作框架：
- `TeamCreateTool` / `TeamDeleteTool`
- `TaskCreate/Update/List/Get` — 共享任务列表
- `SendMessageTool` — 代理间通信
- `AgentTool` — 专门化子代理（Explore, Plan, feature-dev, code-reviewer等）
- 每个代理有独立的 tool set 和 system prompt
- 支持后台运行、worktree 隔离

### 8.2 Orangutan 的现状

有基础的 `SubagentManager` 和三个工具（spawn/status/wait），但：
- 无代理间通信机制
- 无共享任务列表
- 无专门化代理类型定义
- 子代理不能并行执行（当前是同步的 `sync_wait`）

### 8.3 实施方案

**阶段一：异步子代理执行**
- 当前 `SubagentManager::spawn` 是在线程池中同步运行的
- 改为真正的异步：spawn 立即返回，后台线程执行
- wait 工具支持超时轮询

**阶段二：代理间消息传递**
- 新建 `src/messaging/agent-mailbox.hpp`
- 每个 runtime 有一个消息队列
- `SendMessage` 工具让代理可以互相发消息
- inbox 工具已有基础，扩展为通用消息收件箱

**阶段三：专门化代理类型**
- 在配置中定义代理类型及其可用工具集
- 例如 "explore" 类型只有只读工具
- 例如 "code-reviewer" 类型有所有读取工具 + WebSearch

---

## 九、其他重要缺失特性

### 9.1 File State Cache（文件状态缓存）

Claude Code 维护一个 `FileStateCache`，记录每个文件的读取状态，避免重复读取未修改的文件。Orangutan 每次读取都是全量的。

**实施位置：** `src/tools/file-read/file-state-cache.hpp`

### 9.2 CLAUDE.md / 项目上下文文件

Claude Code 自动读取项目目录中的 `CLAUDE.md` 作为项目特定指令。Orangutan 没有等价机制。

**实施方案：** 
- 自动扫描工作目录的 `.orangutan/AGENT.md` 或 `ORANGUTAN.md`
- 内容注入到系统提示词的动态段

### 9.3 输出风格系统

Claude Code 支持 `outputStyles/` 目录自定义输出风格。这允许用户完全重定义 agent 的输出行为。

### 9.4 Structured Output（结构化输出）

Claude Code 的 `SyntheticOutputTool` 支持 JSON Schema 验证的结构化输出，SDK 模式下特别重要。

### 9.5 Git 集成深度

Claude Code 自动收集 git status, branch, default branch, recent commits, user name 并注入到上下文。Orangutan 完全没有 git 感知。

---

## 十、优先级路线图

### P0（立即实施 — 投入/产出比最高）

1. **系统提示词工程化** — 这是最低成本最高收益的改进
   - 从 Claude Code 的 prompts.ts 移植核心指南段
   - 增加环境感知（git/os/cwd）
   - 增加工具使用指导
   - 预计工作量：2-3天

2. **GlobTool + GrepTool** — 解锁"用专用工具而非shell"的提示词指导
   - 预计工作量：1-2天

3. **工具结果微压缩** — 大文件读取结果占满上下文的问题
   - 预计工作量：1天

### P1（近期实施 — 用户体验质变）

4. **Permission Mode 系统** — plan mode 是核心交互模式
5. **AskUserQuestion 工具** — 结构化用户交互
6. **Cost Tracking** — 用户需要知道花了多少钱
7. **增量会话持久化** — 防止进程异常退出丢失数据
8. **Git 状态注入** — 提供代码仓库上下文

### P2（中期实施 — 专业特性）

9. **命令风险分析** — destructive command warning
10. **项目上下文文件** — `.orangutan/AGENT.md`
11. **异步子代理执行** — 并行工作流
12. **文件状态缓存** — 减少重复读取
13. **Prompt Caching** — API 成本优化

### P3（长期实施 — 完整生态）

14. **团队协作系统** — 多代理协作
15. **LSP 集成** — 代码智能
16. **WebFetch/WebSearch** — 网络能力
17. **Structured Output** — SDK场景
18. **输出风格系统** — 自定义输出行为

---

## 十一、Orangutan 的独特优势

在分析差距的同时，也应该看到 Orangutan 已有的差异化优势：

1. **QQ 频道集成** — Claude Code 没有即时通讯集成
2. **Heartbeat 系统** — 主动式健康检查和定期任务
3. **Automation/Cron** — 内置的定时任务系统
4. **多 Agent 配置** — 原生支持多个 agent 配置和工作空间
5. **Secret Protection** — 配置文件加密保护
6. **C++ 性能** — 启动速度和内存占用远优于 Node.js/Bun
7. **Web UI** — 内置 Web 界面
8. **消息队列架构** — channel 模式下的消息处理

这些优势应该保持并强化，而不是在改进过程中丢失。

---

## 十二、总结

**核心差距排序：**

1. **提示词工程** — 差距最大、影响最深、实施成本最低
2. **专用搜索工具** — 解锁提示词中的工具引导
3. **压缩机制** — 长对话稳定性
4. **权限系统** — 安全性和交互体验
5. **API优化** — 成本和性能

最终目标是让 Orangutan 在保持自身 C++ 高性能和多渠道集成优势的同时，在 prompt engineering 和 agent 能力上对齐 Claude Code 的最佳实践。
