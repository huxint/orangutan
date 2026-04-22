# Orangutan 布局骨架 · V1 + V3 左栏（对称三栏 · 卡片式 Contributors）

> 本目录是 **Step 02 · 布局骨架** 的最终落地提示词。
> 视觉真值：[`web/design/02-layout/v1-column.html`](../../design/02-layout/v1-column.html)
> 依赖：[`web/prompt/01-visual-language/`](../01-visual-language/) 的设计令牌
> 本步骤定义：顶栏、左栏、中栏（chat + 发送栏）、右栏（teammate 详情）、分隔条、响应式、键盘、状态机

---

## 一句话骨架

**52px 顶栏 · 260/1fr/320 三栏 · 左栏 Contributors 卡片（含活动预览） · 中栏 720 居中阅读列 · 右栏 push 展开 · 底贴 status + send。**

---

## 布局总视图

```
┌──────────────────── 52px TOP NAV ─────────────────────┐
│ Orangutan · session         tokens · actions · user   │
├─ 260 ─┬─ 1 ─┬──── 1fr ────┬─ 1 ─┬──── 320 ─────────┬─┘
│       │     │             │     │                   │
│ LEFT  │ div │  CHAT SCROLL│ div │  RIGHT PANEL      │
│ (卡片)│     │  (max 720)  │     │  (push on open)   │
│ 每个  │     │             │     │  (0 on close)     │
│ team- │     │             │     │                   │
│ mate  │     │─ STATUS ROW ┤     │                   │
│ 都是  │     │── SEND BAR ─┤     │                   │
│ 独立  │     │             │     │                   │
│ 卡片  │     │             │     │                   │
└───────┴─────┴─────────────┴─────┴───────────────────┘
```

---

## 三条骨架铁律

1. **中栏永远存在、永远能呼吸。** 左栏可压到 220，右栏可收到 0，但中栏 `min-width` 永远 ≥ 560px。
2. **右栏是 push 不是 overlay。** 打开时压缩中栏，关闭时中栏重新扩展——这是 V1 的核心：teammate 详情是"对话的另一面"，不是浮在上的临时工具。
3. **状态栏 + 发送栏永远粘在中栏底部**，跟随中栏宽度。不浮、不跨栏，让"谁在说什么、正在做什么"的信息流聚焦在一个读取列里。

---

## 为什么左栏用卡片而不是行列表

V1 原本的行列表（40×40 avatar + 单行 name + 单行 status）被替换成卡片（含活动预览 + token/cache 指标），**理由**：

1. **用户的原始诉求是"详细看得到在干嘛"。** 行列表只能显示 1 行状态，信息量不够；卡片能显示 2 行活动预览 + 指标，扫一眼就知道谁在干啥。
2. **页面结构不变。** 仍然是对称三栏、push 右栏、底贴发送——只是左栏每个"行"变厚了。
3. **teammate 数 ≤ 8 时体验最佳**；超过后自动 clamp 活动行高（见 `left-panel.md · §7`）。

---

## 文件索引

| 文件 | 用途 |
|---|---|
| [`shell-and-grid.md`](./shell-and-grid.md) | App shell · CSS Grid · 命名区域 · 状态机 · 分隔条 · 响应式断点 |
| [`top-nav.md`](./top-nav.md) | 52px 顶栏的左右两侧结构、操作、API 绑定 |
| [`left-panel.md`](./left-panel.md) | **260px 卡片式** Contributors 列表、活动预览、指标、新增入口、empty state |
| [`middle-chat.md`](./middle-chat.md) | 中栏 chat 滚动容器、status row、send bar、滚动行为（**禁 scrollIntoView**） |
| [`right-panel.md`](./right-panel.md) | 右栏 push 展开、metrics、activity 时间线、thinking、empty state |
| [`interactions.md`](./interactions.md) | 键盘快捷键、分隔拖拽、过渡时序、焦点管理、可达性、自检清单 |

---

## 依赖与边界

**本步骤只定义骨架的结构 / 尺寸 / 状态机 / 交互。**
- 消息块内部渲染（code / diff / math / tool / thinking） → Step C
- 主题切换的圆圈掠过动画 → Step H（但状态变量 `data-theme="light|dark"` 在本步骤已就位）
- 顶栏里每个入口的内容页（Skills / MCP / Memory / Providers / Workspace） → Step G
- 左栏动态新增 teammate 的进入动效细节 → Step D

所有后续步骤**只能在本骨架之上挂载**，不得修改骨架的 grid 结构与状态机。

---

## 落地顺序建议

1. 读 `shell-and-grid.md`，先搭好 app 外壳 + CSS Grid + 空 placeholder 区域
2. 按 `top-nav.md` 实现顶栏，接 `/api/v1/system` 和 `/api/v1/agents/graph`
3. 按 `left-panel.md` 实现 Contributors 卡片列表，订阅 `/api/v1/events`
4. 按 `middle-chat.md` 实现中栏 + 发送栏，接 `POST /api/v1/chat` SSE
5. 按 `right-panel.md` 实现右栏，从 lead 的 tool 流解析 teammate 活动
6. 按 `interactions.md` 接键盘 + 分隔拖拽 + 响应式 + 无障碍

每一步完成后运行 `interactions.md · §9` 的自检表。
