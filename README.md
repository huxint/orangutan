# Orangutan

Orangutan 是一个用 **C++26** 编写的 AI Agent 运行时。它把模型调用、工具执行、权限控制、会话与长期记忆、多 Agent 协作连成一个完整循环，让 Agent 带着上下文、工具和队友持续工作。

你可以把它嵌入自己的 C++ 应用，通过 Telegram 私聊使用它，也可以运行桌面浏览器原型，体验对话、任务、连接、记忆和设置组成的工作空间。

## 它能做什么

- **调用模型**：支持 Anthropic Messages 和 OpenAI Responses，提供流式响应、原生工具调用、重试与模型回退，以及用量统计和费用估算。
- **操作工作区**：通过 `FileRead`、`FileWrite`、`FileEdit` 读写文件；外部操作经过 allow / ask / deny 权限决策，并留下钩子与审计记录。
- **继续对话**：保存原始会话和工作上下文检查点，重新打开后继续；历史较长时压缩模型看到的上下文，保留原始记录供后续查询。
- **使用长期记忆**：按作用域发现、检索和读取事实与偏好，通过记忆工具保存、更正或显式删除记录；笔记更正保留修订历史。
- **分工协作**：按任务创建子 Agent 和专业角色，安排后台任务、审查依赖结果，并复用队友会话进行后续修正；队友之间可以发送定向消息。
- **接入聊天与界面**：提供 Telegram 私聊宿主、QQ / Telegram / 飞书协议适配，以及带中英文和浅色 / 深色主题的桌面浏览器原型。

## 快速开始

### 构建运行时

需要 Linux、GCC 16.1 或更新版本、xmake 3.1.1，以及系统 libcurl 开发文件和 pkg-config。其余依赖由 xmake 解析，版本与说明见 [依赖清单](docs/rules/libraries.md)。

```sh
git clone https://github.com/huxint/orangutan.git
cd orangutan

xmake f -y -m release
xmake build -j4
xmake test -j4
make ci
```

默认构建生成运行时库。Telegram、QQ 登录和评估程序是按需构建的独立目标；完整命令见 [构建说明](docs/BUILD_SYSTEM.md)。

### 体验桌面原型

需要 Node.js 24 或更新版本和 npm：

```sh
npm --prefix apps/desktop ci
npm --prefix apps/desktop run dev
```

打开 <http://127.0.0.1:5173>。原型包含对话、任务、连接、记忆、设置五个页面，支持审批演示、团队任务、会话命名、主题切换和页面关闭后的恢复。

桌面原型使用模拟执行与模拟频道数据，当前独立于 C++ 运行时。使用方式和浏览器验证命令见 [桌面原型 README](apps/desktop/README.md)。

### 通过 Telegram 对话

`oran-telegram` 为一个明确指定的用户提供私聊入口，支持持久会话、流式预览、Markdown、回复引用和图片输入；图片理解需要支持视觉的模型。

先在环境中设置 `ORAN_TELEGRAM_TOKEN` 和 `ORAN_TELEGRAM_MODEL_KEY`，并指定允许的用户、工作区与状态目录：

```sh
xmake build -j4 oran-telegram

build/linux/x86_64/release/oran-telegram \
  --config apps/telegram/deepseek.example.json \
  --allow-user "$TELEGRAM_USER_ID" \
  --workspace "$WORKSPACE_DIR" \
  --state "$STATE_DIR"
```

`/new` 创建新会话，`/status` 查看当前状态，`/tasks` 查看后台任务，`/stop` 请求停止后台任务，`/help` 查看完整命令。

常驻宿主支持后台子任务和 Agent 团队：提交任务后可以继续对话，完成结果会作为后续消息送达。部署、凭据、权限和投递恢复见 [Telegram 宿主说明](docs/design-docs/messaging-channels.md#telegram-deployment-host)。

## 接入自己的应用

链接 `oran-bootstrap`，包含 `<oran/bootstrap.hpp>`，由宿主提供执行器、工作区、会话身份和资源生命周期：

| 接口 | 负责什么 |
| --- | --- |
| `HttpProviderBackend` | 从显式配置构造模型路由和 HTTP 传输。 |
| `RuntimeAssembly` | 装配并持有工作区、权限、钩子和存储服务。 |
| `AgentSession` | 运行提问、读取记忆、执行工具，并保存完成的会话。 |

从 [config.example.json](config.example.json) 开始配置模型和权限。`api_key_env` 指向保存 API key 的环境变量，密钥由宿主在装配时解析。调用 `AgentSession::run_prompt` 后，等待执行与清理完成再释放服务。

[运行时装配](docs/design-docs/bootstrap-runtime.md) 说明接口和资源所有权；[HTTP 会话续接测试](tests/bootstrap/test_provider_backend.cpp) 展示从模型传输到持久会话的完整组合。

聊天接入使用注入式传输和会话接口，各入口的当前边界如下：

| 入口 | 当前提供的能力 |
| --- | --- |
| Telegram | 可直接部署的私聊宿主，包含接收、执行、持久化和投递恢复。 |
| QQ | 官方扫码绑定、凭据探测、认证接口与协议适配；消息入口和投递部署由宿主提供。 |
| 飞书 | 协议适配、消息分发和活动状态；认证入口和持久投递由宿主提供。 |
| 桌面原型 | 浏览器中的模拟工作空间，展示本地操作和频道只读视图。 |

QQ 扫码绑定使用 `oran-qq-login`，构建时需要系统 libqrencode 开发文件。具体操作见 [QQ 授权说明](docs/design-docs/messaging-channels.md#qr-authorization)；所有聊天入口的接口与恢复要求见 [消息频道设计](docs/design-docs/messaging-channels.md)。

## 设计概览

应用入口通过 `oran-bootstrap` 装配模型、工具、权限、会话与存储。运行时的值转换与策略判断接收显式输入，HTTP、文件和 SQLite 操作通过独立的效果边界执行。

![Orangutan 系统架构：应用与聊天入口、运行时装配、模型和工具循环、记忆与协作](docs/assets/architecture-overview.svg)

模块依赖与公共接口见 [架构说明](docs/ARCHITECTURE.md)。

### 一次提问如何完成

会话载入检查点和记忆索引，组织模型上下文并发起请求。模型提出工具调用时，运行时解析输入、检查对应权限或作用域、执行工具，再把类型化结果放回对话，继续下一轮。模型给出最终回答后，完成的原始会话后缀与检查点原子提交。

![Orangutan 消息循环：载入上下文、调用模型、授权并执行工具、继续推理、返回和保存](docs/assets/message-flow.svg)

同一轮提问沿用选定的工具目录和系统前缀。工具调度有并发与时间预算，取消请求会等待相关工作完成清理；外部授权被拒绝时不会执行对应操作。细节见 [Agent 执行](docs/design-docs/agent-platform.md) 和 [工具运行时](docs/design-docs/tool-runtime.md)。

### 会话与长期记忆

模型当前看到的上下文、持久会话历史和跨会话笔记各有自己的用途。上下文压缩保留当前任务，原始会话单独保存；长期记忆通过索引发现相关笔记，再按需读取详细内容。

![Orangutan 记忆系统：工作上下文、原始会话、长期笔记，以及发现、读取、写入与更正](docs/assets/memory-system.svg)

`MemoryRecall` 支持索引浏览、词法检索和精确读取，`MemoryRemember` 保存完整笔记或更正已有记录，`MemoryForget` 处理显式删除。宿主绑定记忆作用域，主 Agent 和子 Agent 共享获准的作用域；查询与写入沿用相同的验证、钩子和审计边界。见 [记忆系统](docs/design-docs/memory-system.md)。

### 子任务与 Agent 团队

`AgentRun` 从具体任务创建独立子会话。默认每轮最多接纳四个子任务，委派深度为一代；子 Agent 继承工作区、模型路由和权限限制。绑定后台任务服务后，任务可以在前台提问结束后继续，并通过状态查询、取消和完成通知与宿主协作。

团队由主 Agent 按需要定义角色、选择工具和安排任务。依赖任务在前置结果被审查通过后进入执行，修正继续使用原队友会话，定向消息在模型边界送达。

![Orangutan Agent 团队：动态角色、任务依赖、结果审查、队友通信与会话复用](docs/assets/orangutan-multi-agent.svg)

常驻 Telegram 宿主已启用团队能力。库宿主可设置 `BackgroundTaskOptions::enable_teams`。任务、团队和待投递的队友消息属于进程内状态，完成的子会话会持久化；生命周期和准入限制见 [Agent 团队](docs/design-docs/agent-teams.md)。

## 测试与开发

```sh
# 完整运行时测试与仓库检查
xmake test -j4
make ci

# 单个模块
xmake build test-agent
xmake run test-agent
```

桌面原型有独立的状态测试、构建、格式检查和浏览器测试，命令见 [桌面验证](apps/desktop/README.md#verify)。`eval-context` 提供可选的上下文保留评估，`eval-qq` 提供有界的 QQ 对话验证，使用与结果解释见 [测试说明](docs/rules/testing-and-bench.md)。

开发前阅读 [AGENTS.md](AGENTS.md)、[当前状态](docs/STATUS.md) 和 [架构](docs/ARCHITECTURE.md)。当前约束由各设计文档维护，未完成的可靠性与验证工作集中在 [技术债务](docs/exec-plans/tech-debt-tracker.md)。

## License

本仓库目前未提供再分发许可。
