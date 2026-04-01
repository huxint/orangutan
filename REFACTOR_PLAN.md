# Orangutan 项目文件树重构计划 v2

> **核心原则**：消灭 `core/`、`infra/`、`app/`、`features/` 等笼统伞目录。
> 每个顶层目录名必须**自解释** —— 看到名字就知道里面装了什么。
> 参考 claude-code 的扁平化领域驱动布局：`commands/`, `tools/`, `hooks/`, `utils/`, `cli/`, `web/` 等。

---

## 一、现状诊断

### 1.1 当前文件树

```
src/
├── main.cpp
├── app/                         # ⚠ 太笼统：混装了 CLI 交互、启动逻辑、会话管理、运行时
│   ├── bootstrap.cpp/hpp        #   CLI 解析 + 初始化（1179 行，职责过重）
│   ├── channel-serve.cpp/hpp
│   ├── cli-ui.cpp/hpp
│   ├── history-events.cpp/hpp
│   ├── line-editor.cpp/hpp
│   ├── repl.cpp/hpp
│   ├── session-workflow.cpp/hpp
│   ├── single-shot.cpp/hpp
│   ├── slash-commands.cpp/hpp
│   └── runtime/
│       ├── agent-runtime.cpp/hpp
│       ├── app-runtime.cpp/hpp
│       ├── identity.cpp/hpp
│       └── memory-context.cpp/hpp
├── core/                        # ⚠ 太笼统："核心" 可以是任何东西
│   ├── types.hpp                #   base alias + Message + 序列化 + ToolDef 全部混在一起
│   ├── providers/               #   LLM 提供者实现
│   └── tools/                   #   工具注册框架
├── features/                    # ⚠ 太笼统："功能" 可以是任何东西
│   ├── agent/
│   ├── automation/
│   ├── channel/{core,qq}/
│   ├── cron/                    #   孤立 — 应属于 automation
│   ├── heartbeat/protocol/      #   多余嵌套
│   ├── hooks/
│   ├── memory/
│   ├── skills/
│   ├── subagent/
│   ├── tools/{builtin,core,mcp,memory,runtime,script,subagent}/  # ⚠ core/ 与顶层 core/ 冲突
│   └── web/
└── infra/                       # ⚠ 太笼统："基础设施" = 配置 + 存储 + 子进程 + 文件 + 杂项
    ├── config/
    ├── execution/               #   只有一个文件
    ├── files/
    ├── storage/
    ├── subprocess/
    ├── time/
    ├── format.hpp               #   散落文件
    ├── string.hpp
    └── utf8.*
```

### 1.2 核心问题

| # | 问题 | 说明 |
|---|------|------|
| P1 | **伞目录不自解释** | `core/`, `infra/`, `app/`, `features/` 看名字猜不出内容，必须深入才能理解 |
| P2 | `core/types.hpp` 单文件塞太多 | base 别名 + 枚举 + Message + Conversation + JSON 序列化 + ToolDef + LLMResponse |
| P3 | `core/tools/tool.hpp` 职责过重 | Tool + ToolRegistry + ToolRuntimeContext + BackgroundCompletion 混在一起 |
| P4 | `features/tools/core/` vs `core/tools/` 命名冲突 | 两个 "core tools" 含义完全不同 |
| P5 | `features/cron/` 孤立 | 逻辑上属于 automation |
| P6 | `features/heartbeat/protocol/` 多余嵌套 | 两层目录只放 4 个文件 |
| P7 | `infra/` 散落文件 + 孤立目录 | `format.hpp`、`string.hpp`、`utf8.*` 无目录；`execution/` 只有一个文件 |
| P8 | `app/bootstrap.cpp` 1100+ 行 | CLI 解析 + 配置构建 + 模式分发全部耦合 |
| P9 | `app/` 混装 CLI UI + 编排 + 会话 + 运行时 | 完全不同的关注点共处一室 |

---

## 二、目标文件树

**设计原则**：
1. 每个顶层目录名直接说明内容（`config/` 不是 `infra/config/`；`cli/` 不是 `app/cli/`）
2. 目录按**领域**划分，不按**架构层**划分
3. 最多两级嵌套（`tools/shell/`），不出现三级
4. 单文件不独占目录（合并或提升）

```
src/
├── main.cpp                           # 入口（不变）
│
├── types/                             # 消息类型、内容块、API 定义
│   ├── base.hpp                       #   类型别名 (f32/i32/u64) + 枚举 (role, origin)
│   ├── content.hpp                    #   Text, Thinking, ToolUse, ToolResult, Content
│   ├── message.hpp                    #   Message, Conversation
│   ├── serialization.hpp              #   content_block_to_json, message_to_json
│   ├── tool-def.hpp                   #   ToolDef, LLMResponse
│   └── types.hpp                      #   聚合头文件（向后兼容）
│
├── providers/                         # LLM 后端（Anthropic, OpenAI, 回退链）
│   ├── provider.hpp                   #   Provider 抽象接口 + ProviderEndpoint
│   ├── provider-factory.cpp           #   create_provider / create_provider_with_fallbacks
│   ├── anthropic-provider.cpp/hpp
│   ├── openai-provider.cpp/hpp
│   ├── sse-parser.cpp/hpp             #   SSE 流解析
│   └── http-client.hpp                #   重命名 http.hpp → 更具描述性
│
├── tools/                             # 工具注册框架 + 全部工具实现
│   ├── registry/                      #   注册框架（原 core/tools/）
│   │   ├── tool.hpp                   #     Tool struct + register_all_tools 声明
│   │   ├── tool-registry.cpp/hpp      #     ToolRegistry 类
│   │   ├── tool-context.hpp           #     ToolRuntimeContext + BackgroundCompletion
│   │   └── permissions.cpp/hpp        #     ToolPermissionSettings, ToolApprovalCallback
│   ├── shell/                         #   Shell 工具（参考 claude-code BashTool/）
│   │   ├── shell-tool.cpp
│   │   └── command-sandbox.cpp/hpp
│   ├── file-read/
│   │   └── file-read-tool.cpp
│   ├── file-write/
│   │   └── file-write-tool.cpp
│   ├── file-edit/
│   │   ├── file-edit-tool.cpp
│   │   └── hashline.cpp/hpp
│   ├── background/
│   │   └── background-completion.cpp/hpp
│   ├── mcp/
│   │   ├── mcp-client.cpp/hpp
│   │   └── mcp-manager.cpp/hpp
│   ├── memory/
│   │   └── memory-tool.cpp
│   ├── subagent/
│   │   └── subagent-tool.cpp
│   ├── automation/
│   │   └── automation-tool-support.cpp/hpp
│   ├── heartbeat/
│   │   └── heartbeat-tool.cpp/hpp
│   ├── inbox/
│   │   └── inbox-tool.cpp/hpp
│   ├── task/
│   │   └── task-tool.cpp/hpp
│   ├── script/
│   │   └── script-loader.cpp/hpp
│   ├── runtime-loader/
│   │   └── runtime-loader.cpp/hpp
│   ├── register.cpp/hpp               #   统一注册（合并 register-builtin + register-core）
│   └── internal.hpp                   #   工具层内部共享头文件
│
├── agent/                             # Agent 循环（ReAct loop）
│   └── agent-loop.cpp/hpp
│
├── memory/                            # 记忆存储、搜索、提取、镜像
│   ├── memory-store.cpp/hpp           #   重命名 memory.* → memory-store.*
│   ├── memory-extract.cpp/hpp
│   ├── memory-search.cpp/hpp
│   ├── memory-schema.cpp/hpp
│   ├── memory-mirror.cpp/hpp
│   └── runtime-memory.cpp/hpp
│
├── automation/                        # 定时任务调度 + Cron 解析
│   ├── scheduler.cpp/hpp              #   重命名 runtime.* → scheduler.*
│   ├── automation-store.cpp/hpp       #   重命名 store.* → automation-store.*
│   ├── automation-types.cpp/hpp       #   重命名 types.* → automation-types.*
│   ├── log-writer.cpp/hpp
│   ├── planner.cpp/hpp
│   └── cron-parser.cpp/hpp            #   ★ 合并：原 features/cron/parser.*
│
├── subagent/                          # 子代理管理
│   └── subagent-manager.cpp/hpp
│
├── hooks/                             # Hook 扩展系统
│   └── hook-manager.cpp/hpp
│
├── skills/                            # 技能加载
│   └── skill-loader.cpp/hpp
│
├── heartbeat/                         # 心跳协议（扁平化，去掉 protocol/ 嵌套）
│   ├── heartbeat-md.cpp/hpp
│   └── heartbeat-ok.cpp/hpp
│
├── channel/                           # 频道集成（抽象 + 实现）
│   ├── channel.cpp/hpp                #   频道抽象
│   ├── allowlist.hpp
│   ├── message-queue.hpp
│   ├── jid-task-runner.cpp/hpp
│   └── qq/                            #   QQ 具体实现
│       ├── qq-channel.cpp/hpp         #     重命名避免与父级同名
│       ├── qq-transport.cpp/hpp
│       └── reconnect-backoff.hpp
│
├── cli/                               # 终端交互（REPL、行编辑、斜杠命令、UI 渲染）
│   ├── repl.cpp/hpp
│   ├── single-shot.cpp/hpp
│   ├── line-editor.cpp/hpp
│   ├── cli-ui.cpp/hpp
│   ├── slash-commands.cpp/hpp
│   ├── session-workflow.cpp/hpp
│   └── history-events.cpp/hpp
│
├── web/                               # Web HTTP 服务器 + REST API + WebSocket
│   ├── web-routes.cpp/hpp
│   ├── web-server.cpp/hpp
│   └── web-types.hpp
│
├── config/                            # TOML 配置解析 + 密钥加密
│   ├── config.cpp/hpp
│   ├── config-detail.hpp
│   ├── config-sections-core.cpp
│   ├── config-sections-integrations.cpp
│   ├── secret-fields.cpp/hpp
│   ├── secret-protection.hpp
│   ├── secret-protection-crypto.cpp
│   ├── secret-protection-file.cpp
│   └── secret-protection-password.cpp
│
├── storage/                           # SQLite 封装 + 持久化存储
│   ├── sqlite.cpp/hpp
│   ├── session-store.cpp/hpp
│   └── subagent-run-store.cpp/hpp
│
├── process/                           # 子进程执行 + 沙箱
│   └── subprocess.cpp/hpp
│
├── bootstrap/                         # 启动初始化 + 运行时组装
│   ├── bootstrap.cpp/hpp              #   CLI 解析 + 模式分发（瘦身后）
│   ├── config-builder.cpp/hpp         #   ★ 新：从 bootstrap 拆出的配置构建
│   ├── agent-runtime.cpp/hpp          #   AgentRuntimeBundle 组装
│   ├── app-runtime.cpp/hpp            #   AppRuntime（automation 持有者）
│   ├── identity.cpp/hpp               #   RuntimeIdentity
│   ├── memory-context.cpp/hpp         #   内存上下文构建
│   └── channel-serve.cpp/hpp          #   频道服务模式入口
│
└── utils/                             # 通用工具函数
    ├── format.hpp                     #   字符串格式化 (append + fmt)
    ├── string.hpp                     #   trim 等字符串操作
    ├── utf8.cpp/hpp                   #   UTF-8 处理
    ├── time-format.hpp                #   时间格式化
    ├── local-time.hpp                 #   本地时间
    ├── file.hpp                       #   RAII 文件句柄
    ├── file-io.cpp/hpp                #   read_file / write_file
    └── sender-utils.hpp               #   异步 sender 工具
```

### 对比总结

| 旧（笼统伞目录） | 新（自解释领域目录） | 为什么更好 |
|-------------------|---------------------|-----------|
| `core/types.hpp` | `types/` (5 文件) | 看到目录名就知道是类型定义 |
| `core/providers/` | `providers/` | 直接表明是 LLM 后端 |
| `core/tools/` + `features/tools/` | `tools/` | 消除命名冲突，注册 + 实现合一 |
| `features/agent/` | `agent/` | 直接说明是 Agent 循环 |
| `features/memory/` | `memory/` | 直接说明是记忆系统 |
| `features/automation/` + `features/cron/` | `automation/` | Cron 回归所属域 |
| `features/heartbeat/protocol/` | `heartbeat/` | 去掉无意义嵌套 |
| `features/channel/` | `channel/` | 不需要 features 前缀 |
| `features/web/` | `web/` | 直接说明是 Web 层 |
| `app/{repl,line-editor,cli-ui,...}` | `cli/` | 纯终端交互，不混编排逻辑 |
| `app/runtime/` + `app/bootstrap` | `bootstrap/` | 启动 + 运行时组装一目了然 |
| `infra/config/` | `config/` | 不需要 infra 前缀 |
| `infra/storage/` | `storage/` | 不需要 infra 前缀 |
| `infra/subprocess/` | `process/` | 更简洁 |
| `infra/{format,string,utf8,time,files,execution}` | `utils/` | 通用工具归一处，无散落文件 |

---

## 三、Namespace 规范

统一策略：**namespace = 顶层目录名**，最多两级。

| 源码目录 | Namespace | 示例 |
|----------|-----------|------|
| `types/` | `orangutan` | `orangutan::Message` |
| `types/` (base) | `orangutan::base` | `orangutan::base::role` |
| `providers/` | `orangutan::providers` | `orangutan::providers::AnthropicProvider` |
| `tools/registry/` | `orangutan::tools` | `orangutan::tools::ToolRegistry` |
| `tools/<name>/` | `orangutan::tools` | `orangutan::tools::execute_shell` |
| `agent/` | `orangutan::agent` | `orangutan::agent::AgentLoop` |
| `memory/` | `orangutan::memory` | `orangutan::memory::MemoryStore` |
| `automation/` | `orangutan::automation` | `orangutan::automation::Scheduler` |
| `subagent/` | `orangutan::subagent` | `orangutan::subagent::SubagentManager` |
| `hooks/` | `orangutan::hooks` | `orangutan::hooks::HookManager` |
| `skills/` | `orangutan::skills` | `orangutan::skills::SkillLoader` |
| `heartbeat/` | `orangutan::heartbeat` | `orangutan::heartbeat::HeartbeatMd` |
| `channel/` | `orangutan::channel` | `orangutan::channel::Channel` |
| `channel/qq/` | `orangutan::channel::qq` | `orangutan::channel::qq::QqChannel` |
| `cli/` | `orangutan::cli` | `orangutan::cli::Repl` |
| `web/` | `orangutan::web` | `orangutan::web::handle_chat` |
| `config/` | `orangutan::config` | `orangutan::config::Config` |
| `storage/` | `orangutan::storage` | `orangutan::storage::SessionStore` |
| `process/` | `orangutan::process` | `orangutan::process::Subprocess` |
| `bootstrap/` | `orangutan::bootstrap` | `orangutan::bootstrap::run` |
| `utils/` | `orangutan::utils` | `orangutan::utils::trim_copy` |
| `utils/` (fileio) | `orangutan::fileio` | `orangutan::fileio::read_file` |
| `utils/` (sqlite) | `orangutan::sqlite` | `orangutan::sqlite::Database` |

> 注意：现有代码中 `orangutan::fileio` 和 `orangutan::sqlite` 已经是独立 namespace，迁移时保持不变。

---

## 四、分步执行计划

> 每一步都是**原子化**的，完成后项目可编译。
> 标注 `[BUILD CHECK]` 的位置必须执行 `xmake build` 验证。
> 对于 namespace 变更，本计划采用**新旧共存 → 逐步迁移**策略，不在一次 commit 中改完所有 namespace。

---

### Phase 0: 准备

```bash
git checkout -b refactor/domain-driven-layout
xmake build -m release   # 确认基线可编译
```

---

### Phase 1: 拆分 `core/types.hpp` → `types/`

**目的**：将 295 行的单体头文件拆成 5 个职责明确的文件。

#### Step 1.1 — 创建目录 + 拆分文件

```bash
mkdir -p src/types
```

创建以下文件（从 `core/types.hpp` 逐段提取）：

| 新文件 | 提取行范围 | 内容 |
|--------|-----------|------|
| `types/base.hpp` | 16-39 | `namespace orangutan::base` 的全部类型别名和枚举 |
| `types/content.hpp` | 42-88 | Text, Thinking, ToolUse, ToolResult, Content variant |
| `types/message.hpp` | 90-248 | Message, Conversation 类 |
| `types/serialization.hpp` | 251-281 | content_block_to_json, message_to_json |
| `types/tool-def.hpp` | 284-295 | ToolDef, LLMResponse |
| `types/types.hpp` | — | 聚合 include（`#include` 以上 5 个文件） |

#### Step 1.2 — 转发旧路径

将 `core/types.hpp` 内容替换为：
```cpp
#pragma once
#include "types/types.hpp"
```

**[BUILD CHECK]** — 零改动编译验证。

#### Step 1.3 — 全局替换 include

搜索所有 `#include "core/types.hpp"`，替换为具体需要的子文件或 `"types/types.hpp"`。

#### Step 1.4 — 删除旧文件

```bash
rm src/core/types.hpp
```

**[BUILD CHECK]**

---

### Phase 2: 提升 `core/providers/` → `providers/`

#### Step 2.1 — 移动文件

```bash
mkdir -p src/providers
mv src/core/providers/provider.hpp         src/providers/
mv src/core/providers/anthropic-provider.* src/providers/
mv src/core/providers/openai-provider.*    src/providers/
mv src/core/providers/provider-factory.cpp src/providers/
mv src/core/providers/sse-parser.*         src/providers/
mv src/core/providers/http.hpp             src/providers/http-client.hpp
```

#### Step 2.2 — 更新内部 include + 全局替换

- provider 文件内部：`"core/types.hpp"` → `"types/types.hpp"`
- 全局：`"core/providers/"` → `"providers/"`
- `"core/providers/http.hpp"` → `"providers/http-client.hpp"`

**[BUILD CHECK]**

---

### Phase 3: 拆分 `core/tools/` → `tools/registry/`

#### Step 3.1 — 创建目录 + 拆分 tool.hpp

```bash
mkdir -p src/tools/registry
```

从 `core/tools/tool.hpp` 拆分为 3 个文件：

| 新文件 | 内容 |
|--------|------|
| `tools/registry/tool-registry.hpp` | Tool struct, ToolRegistry class, scrub_tool_output, register_builtin_tools |
| `tools/registry/tool-context.hpp` | ToolRuntimeContext, BackgroundCompletionRuntimeBindings, make_background_completion_runtime_bindings |
| `tools/registry/tool.hpp` | 聚合 include（兼容旧代码） |

#### Step 3.2 — 移动权限 + 注册表实现

```bash
mv src/core/tools/permissions.cpp src/tools/registry/
mv src/core/tools/permissions.hpp src/tools/registry/
mv src/core/tools/registry.cpp    src/tools/registry/tool-registry.cpp
```

#### Step 3.3 — 全局替换 include

- `"core/tools/tool.hpp"` → `"tools/registry/tool.hpp"` 或具体子文件
- `"core/tools/permissions.hpp"` → `"tools/registry/permissions.hpp"`

#### Step 3.4 — 删除 `src/core/`

```bash
rm -rf src/core/
```

**[BUILD CHECK]**

---

### Phase 4: 重组工具实现 → `tools/<name>/`

**目的**：消除 `features/tools/core/` 歧义，每个工具一个自解释目录。

#### Step 4.1 — 移动核心工具

```bash
mkdir -p src/tools/{shell,file-read,file-write,file-edit,background}

mv src/features/tools/core/shell.cpp             src/tools/shell/shell-tool.cpp
mv src/features/tools/core/command-sandbox.*      src/tools/shell/
mv src/features/tools/core/read.cpp              src/tools/file-read/file-read-tool.cpp
mv src/features/tools/core/write.cpp             src/tools/file-write/file-write-tool.cpp
mv src/features/tools/core/edit.cpp              src/tools/file-edit/file-edit-tool.cpp
mv src/features/tools/core/hashline.*            src/tools/file-edit/
mv src/features/tools/core/background-completion.* src/tools/background/
mv src/features/tools/core/internal.hpp          src/tools/internal.hpp
mv src/features/tools/core/register-core.cpp     src/tools/register-core.cpp
```

#### Step 4.2 — 移动 builtin 工具

```bash
mkdir -p src/tools/{automation,heartbeat,inbox,task}

mv src/features/tools/builtin/automation-tool-support.* src/tools/automation/
mv src/features/tools/builtin/heartbeat.cpp   src/tools/heartbeat/heartbeat-tool.cpp
mv src/features/tools/builtin/heartbeat.hpp   src/tools/heartbeat/heartbeat-tool.hpp
mv src/features/tools/builtin/inbox.cpp       src/tools/inbox/inbox-tool.cpp
mv src/features/tools/builtin/inbox.hpp       src/tools/inbox/inbox-tool.hpp
mv src/features/tools/builtin/task.cpp        src/tools/task/task-tool.cpp
mv src/features/tools/builtin/task.hpp        src/tools/task/task-tool.hpp
mv src/features/tools/builtin/register-builtin.* src/tools/
```

#### Step 4.3 — 移动 MCP / memory / subagent / script / runtime 工具

```bash
mkdir -p src/tools/{mcp,memory,subagent,script,runtime-loader}

mv src/features/tools/mcp/client.*    src/tools/mcp/mcp-client.cpp
mv src/features/tools/mcp/client.hpp  src/tools/mcp/mcp-client.hpp
mv src/features/tools/mcp/manager.*   src/tools/mcp/mcp-manager.cpp
mv src/features/tools/mcp/manager.hpp src/tools/mcp/mcp-manager.hpp

mv src/features/tools/memory/memory-tools.cpp    src/tools/memory/memory-tool.cpp
mv src/features/tools/subagent/subagent-tools.cpp src/tools/subagent/subagent-tool.cpp
mv src/features/tools/script/*   src/tools/script/
mv src/features/tools/runtime/*  src/tools/runtime-loader/
```

#### Step 4.4 — 合并注册入口

将 `register-builtin.cpp` + `register-core.cpp` 合并为 `src/tools/register.cpp`：
```cpp
// src/tools/register.hpp
#pragma once
namespace orangutan::tools {
    void register_all(ToolRegistry &, ...);
}
```

#### Step 4.5 — 更新 include + 删除旧目录

```bash
rm -rf src/features/tools/
```

全局替换：`"features/tools/..."` → `"tools/..."`

**[BUILD CHECK]**

---

### Phase 5: 提升领域模块为顶层（消灭 `features/`）

**目的**：将 `features/xxx/` 直接提升为 `src/xxx/`。

#### Step 5.1 — agent

```bash
mv src/features/agent/* src/agent/
# 只有 agent-loop.cpp/hpp
```

#### Step 5.2 — memory

```bash
mkdir -p src/memory
mv src/features/memory/memory.cpp       src/memory/memory-store.cpp
mv src/features/memory/memory.hpp       src/memory/memory-store.hpp
mv src/features/memory/memory-extract.* src/memory/
mv src/features/memory/memory-search.*  src/memory/
mv src/features/memory/memory-schema.*  src/memory/
mv src/features/memory/memory-mirror.*  src/memory/
mv src/features/memory/runtime-memory.* src/memory/
```

#### Step 5.3 — automation（合并 cron）

```bash
mkdir -p src/automation
mv src/features/automation/runtime.*    src/automation/scheduler.cpp
mv src/features/automation/runtime.hpp  src/automation/scheduler.hpp
mv src/features/automation/store.*      src/automation/automation-store.cpp
mv src/features/automation/store.hpp    src/automation/automation-store.hpp
mv src/features/automation/types.*      src/automation/automation-types.cpp
mv src/features/automation/types.hpp    src/automation/automation-types.hpp
mv src/features/automation/log-writer.* src/automation/
mv src/features/automation/planner.*    src/automation/

# ★ 合并 cron
mv src/features/cron/parser.cpp src/automation/cron-parser.cpp
mv src/features/cron/parser.hpp src/automation/cron-parser.hpp
```

#### Step 5.4 — subagent, hooks, skills

```bash
mv src/features/subagent/* src/subagent/
mv src/features/hooks/*    src/hooks/
mv src/features/skills/*   src/skills/
```

#### Step 5.5 — heartbeat（扁平化）

```bash
mkdir -p src/heartbeat
mv src/features/heartbeat/protocol/heartbeat-md.* src/heartbeat/
mv src/features/heartbeat/protocol/heartbeat-ok.* src/heartbeat/
```

#### Step 5.6 — channel

```bash
mkdir -p src/channel/qq
mv src/features/channel/core/channel.*          src/channel/
mv src/features/channel/core/allowlist.hpp       src/channel/
mv src/features/channel/core/message-queue.hpp   src/channel/
mv src/features/channel/core/jid-task-runner.*   src/channel/
mv src/features/channel/qq/channel.cpp           src/channel/qq/qq-channel.cpp
mv src/features/channel/qq/channel.hpp           src/channel/qq/qq-channel.hpp
mv src/features/channel/qq/transport.cpp         src/channel/qq/qq-transport.cpp
mv src/features/channel/qq/transport.hpp         src/channel/qq/qq-transport.hpp
mv src/features/channel/qq/reconnect-backoff.hpp src/channel/qq/
```

#### Step 5.7 — web

```bash
mkdir -p src/web
mv src/features/web/* src/web/
```

#### Step 5.8 — 删除 `features/`

```bash
rm -rf src/features/
```

#### Step 5.9 — 全局替换 include

搜索替换所有 `"features/xxx"` → `"xxx"` 引用。

关键替换对照：
```
"features/agent/agent-loop.hpp"          → "agent/agent-loop.hpp"
"features/memory/memory.hpp"             → "memory/memory-store.hpp"
"features/memory/runtime-memory.hpp"     → "memory/runtime-memory.hpp"
"features/automation/runtime.hpp"        → "automation/scheduler.hpp"
"features/automation/store.hpp"          → "automation/automation-store.hpp"
"features/automation/types.hpp"          → "automation/automation-types.hpp"
"features/cron/parser.hpp"              → "automation/cron-parser.hpp"
"features/subagent/subagent-manager.hpp" → "subagent/subagent-manager.hpp"
"features/hooks/hook-manager.hpp"        → "hooks/hook-manager.hpp"
"features/skills/skill-loader.hpp"       → "skills/skill-loader.hpp"
"features/heartbeat/protocol/..."        → "heartbeat/..."
"features/channel/core/..."             → "channel/..."
"features/channel/qq/channel.hpp"        → "channel/qq/qq-channel.hpp"
"features/channel/qq/transport.hpp"      → "channel/qq/qq-transport.hpp"
"features/web/..."                      → "web/..."
```

**[BUILD CHECK]**

---

### Phase 6: 提升 `infra/` 子目录为顶层（消灭 `infra/`）

#### Step 6.1 — config

```bash
mv src/infra/config/* src/config/
# 已经 mkdir -p src/config 在上面的目标树中
```

#### Step 6.2 — storage

```bash
mv src/infra/storage/* src/storage/
```

#### Step 6.3 — subprocess → process

```bash
mkdir -p src/process
mv src/infra/subprocess/* src/process/
```

#### Step 6.4 — 散落文件 + time + files + execution → utils

```bash
mkdir -p src/utils
mv src/infra/format.hpp              src/utils/
mv src/infra/string.hpp              src/utils/
mv src/infra/utf8.*                  src/utils/
mv src/infra/time/local-format.hpp   src/utils/time-format.hpp
mv src/infra/time/local-time.hpp     src/utils/local-time.hpp
mv src/infra/files/file.hpp          src/utils/
mv src/infra/files/file-io.*         src/utils/
mv src/infra/execution/sender-utils.hpp src/utils/
```

#### Step 6.5 — 删除 `infra/`

```bash
rm -rf src/infra/
```

#### Step 6.6 — 全局替换 include

```
"infra/config/config.hpp"              → "config/config.hpp"
"infra/config/config-detail.hpp"       → "config/config-detail.hpp"
"infra/config/secret-protection.hpp"   → "config/secret-protection.hpp"
"infra/config/secret-fields.hpp"       → "config/secret-fields.hpp"
"infra/storage/sqlite.hpp"             → "storage/sqlite.hpp"
"infra/storage/session-store.hpp"      → "storage/session-store.hpp"
"infra/storage/subagent-run-store.hpp" → "storage/subagent-run-store.hpp"
"infra/subprocess/subprocess.hpp"      → "process/subprocess.hpp"
"infra/format.hpp"                     → "utils/format.hpp"
"infra/string.hpp"                     → "utils/string.hpp"
"infra/utf8.hpp"                       → "utils/utf8.hpp"
"infra/time/local-format.hpp"          → "utils/time-format.hpp"
"infra/time/local-time.hpp"            → "utils/local-time.hpp"
"infra/files/file.hpp"                 → "utils/file.hpp"
"infra/files/file-io.hpp"              → "utils/file-io.hpp"
"infra/execution/sender-utils.hpp"     → "utils/sender-utils.hpp"
```

**[BUILD CHECK]**

---

### Phase 7: 拆分 `app/` → `cli/` + `bootstrap/`（消灭 `app/`）

#### Step 7.1 — 创建 cli/ 并移入 CLI 交互文件

```bash
mkdir -p src/cli
mv src/app/repl.*             src/cli/
mv src/app/single-shot.*      src/cli/
mv src/app/line-editor.*      src/cli/
mv src/app/cli-ui.*           src/cli/
mv src/app/slash-commands.*    src/cli/
mv src/app/session-workflow.*  src/cli/
mv src/app/history-events.*   src/cli/
```

#### Step 7.2 — 创建 bootstrap/ 并移入启动 + 运行时文件

```bash
mkdir -p src/bootstrap
mv src/app/bootstrap.*           src/bootstrap/
mv src/app/channel-serve.*       src/bootstrap/
mv src/app/runtime/agent-runtime.* src/bootstrap/
mv src/app/runtime/app-runtime.*   src/bootstrap/
mv src/app/runtime/identity.*      src/bootstrap/
mv src/app/runtime/memory-context.* src/bootstrap/
```

#### Step 7.3 — 从 bootstrap.cpp 提取 config-builder

创建 `src/bootstrap/config-builder.cpp/hpp`，将以下函数从 bootstrap.cpp 移出：
- `build_effective_agents()`
- `resolve_api_key()`
- `build_agent_runtime_configs()`
- `build_subagent_child_runtime_configs()`

瘦身后的 `bootstrap.cpp` 只保留：
1. CLI 参数解析（CLI11）
2. 日志初始化
3. 配置加载 → 调用 config-builder
4. 模式分发（repl / single-shot / web / channel）

#### Step 7.4 — 删除 `app/`

```bash
rm -rf src/app/
```

#### Step 7.5 — 更新 main.cpp

```cpp
#include "bootstrap/bootstrap.hpp"

int main(int argc, char **argv) {
    return orangutan::bootstrap::run(argc, argv);
}
```

#### Step 7.6 — 全局替换 include

```
"app/bootstrap.hpp"           → "bootstrap/bootstrap.hpp"
"app/channel-serve.hpp"       → "bootstrap/channel-serve.hpp"
"app/repl.hpp"                → "cli/repl.hpp"
"app/single-shot.hpp"         → "cli/single-shot.hpp"
"app/line-editor.hpp"         → "cli/line-editor.hpp"
"app/cli-ui.hpp"              → "cli/cli-ui.hpp"
"app/slash-commands.hpp"       → "cli/slash-commands.hpp"
"app/session-workflow.hpp"     → "cli/session-workflow.hpp"
"app/history-events.hpp"       → "cli/history-events.hpp"
"app/runtime/agent-runtime.hpp" → "bootstrap/agent-runtime.hpp"
"app/runtime/app-runtime.hpp"   → "bootstrap/app-runtime.hpp"
"app/runtime/identity.hpp"      → "bootstrap/identity.hpp"
"app/runtime/memory-context.hpp" → "bootstrap/memory-context.hpp"
```

**[BUILD CHECK]**

---

### Phase 8: 更新构建系统

#### Step 8.1 — 更新 `xmake/targets.lua`

```lua
local root = os.projectdir()

local function add_orangutan_common()
    add_includedirs(path.join(root, "src"), {public = true})
    add_packages("cli11", "nlohmann_json", "spdlog", "libcurl", "sqlite3",
                 "toml++", "cpp-httplib", "stdexec", "rapidhash", "replxx",
                 "mbedtls", "simdutf", "uni_algo", "ctre", "magic_enum",
                 {public = true})
    add_syslinks("pthread", {public = true})
    if has_config("qq_channel") then
        add_defines("ORANGUTAN_ENABLE_QQ_CHANNEL=1", {public = true})
    end
end

target("orangutan-lib")
    set_kind("static")
    add_orangutan_common()
    add_files(
        path.join(root, "src/providers/**/*.cpp"),
        path.join(root, "src/tools/**/*.cpp"),
        path.join(root, "src/agent/**/*.cpp"),
        path.join(root, "src/memory/**/*.cpp"),
        path.join(root, "src/automation/**/*.cpp"),
        path.join(root, "src/subagent/**/*.cpp"),
        path.join(root, "src/hooks/**/*.cpp"),
        path.join(root, "src/skills/**/*.cpp"),
        path.join(root, "src/heartbeat/**/*.cpp"),
        path.join(root, "src/channel/**/*.cpp"),
        path.join(root, "src/cli/**/*.cpp"),
        path.join(root, "src/web/**/*.cpp"),
        path.join(root, "src/config/**/*.cpp"),
        path.join(root, "src/storage/**/*.cpp"),
        path.join(root, "src/process/**/*.cpp"),
        path.join(root, "src/bootstrap/**/*.cpp"),
        path.join(root, "src/utils/**/*.cpp")
    )

target("orangutan")
    set_kind("binary")
    add_deps("orangutan-lib")
    add_files(path.join(root, "src/main.cpp"))
```

#### Step 8.2 — 更新 `xmake/tests.lua`

```lua
add_test_target("test-types",      rooted("tests/types/*.cpp"))
add_test_target("test-providers",  rooted("tests/providers/*.cpp"))

add_test_target("test-tools", {
    rooted("tests/tools/**/*.cpp"),
})

add_test_target("test-agent",      rooted("tests/agent/*.cpp"))
add_test_target("test-memory",     rooted("tests/memory/*.cpp"))
add_test_target("test-automation", rooted("tests/automation/*.cpp"))
add_test_target("test-subagent",   rooted("tests/subagent/*.cpp"))
add_test_target("test-heartbeat",  rooted("tests/heartbeat/*.cpp"))

add_test_target("test-channel", {
    rooted("tests/channel/*.cpp"),
    rooted("tests/channel/qq/*.cpp"),
})

add_test_target("test-misc-services", {
    rooted("tests/hooks/*.cpp"),
    rooted("tests/skills/*.cpp"),
})

add_test_target("test-cli",        rooted("tests/cli/*.cpp"))
add_test_target("test-web",        rooted("tests/web/*.cpp"))
add_test_target("test-config",     rooted("tests/config/*.cpp"))
add_test_target("test-storage",    rooted("tests/storage/*.cpp"))
add_test_target("test-process",    rooted("tests/process/*.cpp"))
add_test_target("test-bootstrap",  rooted("tests/bootstrap/*.cpp"))
add_test_target("test-integration", rooted("tests/integration/*.cpp"))
```

#### Step 8.3 — 移动测试文件（镜像新源码结构）

```bash
# types/
mkdir -p tests/types
mv tests/core/types-test.cpp tests/types/

# providers/
mkdir -p tests/providers
mv tests/core/provider-fallback-test.cpp tests/providers/
mv tests/core/sse-parser-test.cpp        tests/providers/

# tools/
mkdir -p tests/tools/{registry,shell,file-edit,mcp,script,heartbeat,inbox,task}
mv tests/core/tool-registry-test.cpp              tests/tools/registry/
mv tests/features/background-shell-completion-test.cpp tests/tools/shell/
mv tests/features/hashline-test.cpp                tests/tools/file-edit/
mv tests/features/mcp-client-test.cpp              tests/tools/mcp/
mv tests/features/script-tool-test.cpp             tests/tools/script/
mv tests/features/heartbeat-tool-test.cpp          tests/tools/heartbeat/
mv tests/features/inbox-tool-test.cpp              tests/tools/inbox/
mv tests/features/task-tool-test.cpp               tests/tools/task/

# agent/
mkdir -p tests/agent
mv tests/features/agent-loop-test.cpp tests/agent/

# memory/
mkdir -p tests/memory
mv tests/features/memory-test.cpp tests/memory/

# automation/
mkdir -p tests/automation
mv tests/features/automation-runtime-test.cpp tests/automation/
mv tests/features/automation-store-test.cpp   tests/automation/
mv tests/features/cron-parser-test.cpp        tests/automation/

# subagent/
mkdir -p tests/subagent
mv tests/features/subagent-manager-test.cpp tests/subagent/
mv tests/features/subagent-tools-test.cpp   tests/subagent/

# hooks/ + skills/
mkdir -p tests/hooks tests/skills
mv tests/features/hook-manager-test.cpp  tests/hooks/
mv tests/features/skill-loader-test.cpp  tests/skills/

# heartbeat/
mkdir -p tests/heartbeat
mv tests/features/heartbeat-md-test.cpp tests/heartbeat/
mv tests/features/heartbeat-ok-test.cpp tests/heartbeat/

# channel/
mkdir -p tests/channel/qq
mv tests/features/channel-test.cpp           tests/channel/
mv tests/features/jid-task-runner-test.cpp   tests/channel/
mv tests/features/channel/qq/reconnect-backoff-test.cpp tests/channel/qq/

# web/
mkdir -p tests/web
mv tests/features/web-server-test.cpp  tests/web/
mv tests/features/web-routes-test.cpp  tests/web/
mv tests/features/web-chat-test.cpp    tests/web/

# cli/ (原 tests/app/)
mkdir -p tests/cli tests/bootstrap
mv tests/app/repl-line-editor-test.cpp    tests/cli/
mv tests/app/cli-ui-test.cpp             tests/cli/
mv tests/app/slash-commands-test.cpp      tests/cli/
mv tests/app/session-workflow-test.cpp    tests/cli/
mv tests/app/history-events-test.cpp      tests/cli/
mv tests/app/single-shot-test.cpp         tests/cli/
mv tests/app/bootstrap-test.cpp           tests/bootstrap/
mv tests/app/bootstrap-web-test.cpp       tests/bootstrap/
mv tests/app/channel-serve-test.cpp       tests/bootstrap/
mv tests/app/identity-test.cpp            tests/bootstrap/
mv tests/app/runtime-agent-runtime-test.cpp tests/bootstrap/

# config/ + storage/ + process/
mkdir -p tests/config tests/storage tests/process
mv tests/infra/config-test.cpp        tests/config/
mv tests/infra/config-save-test.cpp   tests/config/
mv tests/infra/config-secret-test.cpp tests/config/
mv tests/infra/session-store-test.cpp tests/storage/
mv tests/infra/sqlite-test.cpp        tests/storage/
mv tests/infra/subagent-run-store-test.cpp tests/storage/
mv tests/infra/subprocess-test.cpp    tests/process/

# 时间测试
mv tests/core/local-format-test.cpp   tests/utils/ 2>/dev/null || true
mv tests/core/local-time-test.cpp     tests/utils/ 2>/dev/null || true

# 清理旧目录
rm -rf tests/core/ tests/features/ tests/app/ tests/infra/
```

#### Step 8.4 — 更新测试文件中的 include 路径

**[BUILD CHECK]** — 全部测试编译通过。

---

### Phase 9: 最终清理

#### Step 9.1 — 删除空目录

```bash
find src/ tests/ -type d -empty -delete
```

#### Step 9.2 — 验证无旧路径残留

```bash
grep -rn '"core/'     src/ tests/ && echo "FAIL: core/ references remain" || echo "OK"
grep -rn '"features/' src/ tests/ && echo "FAIL: features/ references remain" || echo "OK"
grep -rn '"infra/'    src/ tests/ && echo "FAIL: infra/ references remain" || echo "OK"
grep -rn '"app/'      src/ tests/ && echo "FAIL: app/ references remain" || echo "OK"
```

#### Step 9.3 — 完整编译 + 全部测试

```bash
xmake clean && xmake build -m release
# 运行所有测试目标
```

#### Step 9.4 — 更新 compile_commands.json

```bash
xmake project -k compile_commands
```

---

## 五、完整重命名对照表

| 旧路径 | 新路径 | 原因 |
|--------|--------|------|
| `core/types.hpp` | `types/` (5 文件 + 聚合) | 拆分单体文件 |
| `core/providers/` | `providers/` | 消灭 core/ |
| `core/providers/http.hpp` | `providers/http-client.hpp` | 更具描述性 |
| `core/tools/` | `tools/registry/` | 消灭 core/，明确是注册框架 |
| `core/tools/registry.cpp` | `tools/registry/tool-registry.cpp` | 匹配类名 |
| `features/tools/core/shell.cpp` | `tools/shell/shell-tool.cpp` | 消灭歧义 core/ |
| `features/tools/core/read.cpp` | `tools/file-read/file-read-tool.cpp` | 自解释 |
| `features/tools/core/write.cpp` | `tools/file-write/file-write-tool.cpp` | 自解释 |
| `features/tools/core/edit.cpp` | `tools/file-edit/file-edit-tool.cpp` | 自解释 |
| `features/tools/builtin/heartbeat.*` | `tools/heartbeat/heartbeat-tool.*` | 统一命名 |
| `features/tools/builtin/inbox.*` | `tools/inbox/inbox-tool.*` | 统一命名 |
| `features/tools/builtin/task.*` | `tools/task/task-tool.*` | 统一命名 |
| `features/tools/mcp/client.*` | `tools/mcp/mcp-client.*` | 加域前缀 |
| `features/tools/mcp/manager.*` | `tools/mcp/mcp-manager.*` | 加域前缀 |
| `features/agent/` | `agent/` | 消灭 features/ |
| `features/memory/memory.*` | `memory/memory-store.*` | 区分文件和模块 |
| `features/automation/runtime.*` | `automation/scheduler.*` | 避免 runtime 歧义 |
| `features/automation/store.*` | `automation/automation-store.*` | 加域前缀 |
| `features/automation/types.*` | `automation/automation-types.*` | 加域前缀 |
| `features/cron/parser.*` | `automation/cron-parser.*` | 归入所属域 |
| `features/heartbeat/protocol/*` | `heartbeat/*` | 去嵌套 |
| `features/channel/core/*` | `channel/*` | 去嵌套 |
| `features/channel/qq/channel.*` | `channel/qq/qq-channel.*` | 避免父子同名 |
| `features/channel/qq/transport.*` | `channel/qq/qq-transport.*` | 加域前缀 |
| `features/web/*` | `web/*` | 消灭 features/ |
| `infra/config/` | `config/` | 消灭 infra/ |
| `infra/storage/` | `storage/` | 消灭 infra/ |
| `infra/subprocess/` | `process/` | 消灭 infra/，更简洁 |
| `infra/files/*` | `utils/{file,file-io}.*` | 归入工具类 |
| `infra/format.hpp` | `utils/format.hpp` | 归入工具类 |
| `infra/string.hpp` | `utils/string.hpp` | 归入工具类 |
| `infra/utf8.*` | `utils/utf8.*` | 归入工具类 |
| `infra/time/local-format.hpp` | `utils/time-format.hpp` | 归入工具类 + 重命名 |
| `infra/time/local-time.hpp` | `utils/local-time.hpp` | 归入工具类 |
| `infra/execution/sender-utils.hpp` | `utils/sender-utils.hpp` | 消除孤立目录 |
| `app/repl.*` | `cli/repl.*` | 消灭 app/，归入 CLI |
| `app/single-shot.*` | `cli/single-shot.*` | 消灭 app/，归入 CLI |
| `app/line-editor.*` | `cli/line-editor.*` | 消灭 app/，归入 CLI |
| `app/cli-ui.*` | `cli/cli-ui.*` | 消灭 app/，归入 CLI |
| `app/slash-commands.*` | `cli/slash-commands.*` | 消灭 app/，归入 CLI |
| `app/session-workflow.*` | `cli/session-workflow.*` | 消灭 app/，归入 CLI |
| `app/history-events.*` | `cli/history-events.*` | 消灭 app/，归入 CLI |
| `app/bootstrap.*` | `bootstrap/bootstrap.*` | 消灭 app/ |
| `app/channel-serve.*` | `bootstrap/channel-serve.*` | 消灭 app/ |
| `app/runtime/*` | `bootstrap/*` | 运行时组装属于启动阶段 |

---

## 六、风险与缓解

| 风险 | 缓解 |
|------|------|
| include 路径大量变更 | 每个 Phase 末 BUILD CHECK；用 `grep -rn` 扫旧路径 |
| 编译级联失败 | Phase 1-3 用转发头文件过渡 |
| 测试硬编码路径 | 扫描 `SOURCE_DIR` 宏和 `#include` |
| xmake glob 匹配变化 | 使用 `**/*.cpp` 递归通配 |
| namespace 不一致 | 本次只做文件移动；namespace 统一作为可选后续步骤 |

---

## 七、后续可选优化

1. **Namespace 统一** — 按第三节规范逐步迁移（可独立 PR）
2. **web-routes.cpp 拆分** — 1231 行按资源拆为 `session-routes.cpp` / `chat-routes.cpp` / `admin-routes.cpp`
3. **bootstrap.cpp 进一步拆分** — 如瘦身后仍超 500 行
4. **工具自注册模式** — 每个 `tools/<name>/` 提供 `register()` 函数
5. **C++20 modules** — `types/`、`providers/`、`tools/` 作为 module partitions

---

*文档版本: v2.0 | 生成日期: 2026-04-01*
