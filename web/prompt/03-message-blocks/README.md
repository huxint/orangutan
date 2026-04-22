# Orangutan 消息块渲染 · V3 · Workshop（工具台）

> 本目录是 **Step 03 · 消息块渲染** 的最终落地提示词。
> 任何实现 Orangutan 中栏消息渲染的开发者或 agent，应先读本目录，再动手。
> 视觉真值 ground truth：[`web/design/03-message-blocks/v3-workshop.html`](../../design/03-message-blocks/v3-workshop.html)

---

## 一句话气质

**"每个块都是可操作的工件。" · bar + body · 依产出/证据区分默认展开策略 · 中性 chip · 线性图标**

消息流里每一个 non-trivial 块（code / diff / math / tool / thinking / image）都有一条 **38px 顶部 bar**（身份信息 + 操作图标），下面挂可见或折叠的 body。bar 是"这是什么"，body 是"内容"。当内容是 **lead 的主要可视产出**（code / diff / math / image）默认展开；当是 **调用证据或辅助思考**（tool / thinking）默认折叠，点击才打开。

区别于 V1 手稿边注（quiet）和 V2 卷注（labeled）：V3 接纳"这是一个工作台"的定位——代码、diff、工具是**工件**，bar 上有真实可点的图标按钮；但配色、字体、阴影**全程沿用 Step 01 的手稿视觉语言**（oklch 暖纸 / Newsreader + Spline Sans Mono / 浅油墨阴影 / 软圆角）。不是 VSCode 克隆，是**手稿里的工程附录**。

---

## 依赖与边界

### 依赖
- [`web/prompt/01-visual-language/`](../01-visual-language/) —— 所有 token、字体、颜色、阴影、圆角的唯一来源
- [`web/prompt/02-layout/`](../02-layout/) —— middle-chat 的滚动容器、消息 shell（`.msg` / `.avatar`）、顶栏结构

### 本步骤范围
- 在 `.msg__body` 内部的所有块级/行内内容渲染
- 每块的 bar（toolbar）原子：chip · 名称 · meta · 状态 dot · icon 按钮
- 每块的 body 渲染：代码高亮 / diff 视图 / KaTeX math / tool 输出 / thinking 折叠
- markdown 文本规范化（含 math 3 种界定符的规范化）
- 块之间的默认折叠策略与交互（单击展开 / toggle view）

### 不在本步骤
- 左栏 teammate 动态接入动画 → **Step D**
- 右栏 teammate 详情时间线（含 teammate thinking） → **Step E**
- 表情贴图 / "正在打字"指示 / 发送栏附件 / 状态信息条 → **Step F**
- 顶栏配置页面（Skills / MCP / Memory / Providers / Workspace） → **Step G**
- 主题圆圈掠过动画 → **Step H**
- Orangutan 动态 SVG（用 Step 01 的 placeholder） → **Step I**

---

## 文件索引

| 文件 | 用途 |
|---|---|
| [`block-anatomy.md`](./block-anatomy.md) | 共享 `.block` 外壳、`.block__bar` 结构、chip / icon / dot 原子、`.is-open` 状态机、默认展开策略表、图标库选择 |
| [`text-markdown.md`](./text-markdown.md) | `.msg__body` 文本基线、markdown → HTML 渲染、段落/列表/表格/标题/块引用/分隔线/链接/inline code/emphasis、SmartyPants 关停 |
| [`code-and-diff.md`](./code-and-diff.md) | 代码块（语法高亮 token / 行号 gutter / chrome / 复制）· 块 diff（unified + split toggle · hunk · +/− 三重冗余）· 行内 diff |
| [`math.md`](./math.md) | 三种输入界定符（DollarBlock `$$…$$` / DelimitedInline `\(…\)` / DelimitedBlock `\[…\]`）的规范化规则与 KaTeX 渲染、wrapper 样式、fallback、可访问性 |
| [`specialty-blocks.md`](./specialty-blocks.md) | Tool 块（默认折叠 · 流式 dot · 结构化输出探测 · 连续调用合并）· Thinking 块（仅 lead · 流式 · 默认折叠）· Reply-Quote（手稿注脚 + 放大引号 · 跳回原文）· Image 块（frame / caption / 生成中） |

---

## 三条铁律（所有块都必须守的）

1. **所有颜色 / 字号 / 圆角 / 阴影 / 间距**引用 Step 01 tokens。**禁止在块样式里硬编码色值**。
2. **所有折叠/展开**用 `.is-open` class 切 `display`；**禁用 `max-height` + opacity 渐变模拟"平滑过渡"**——简单 CSS `transition` 足够，否则会与 markdown 重排打架。
3. **禁用 `scrollIntoView`**（Step 02 铁律的延续）。跳回原消息用 `container.scrollTop = target.offsetTop - container.offsetTop`。

---

## 默认展开策略 · 一屏速查

| 块 | bar | body 默认 | 原因 |
|---|---|---|---|
| Text / Markdown | 无 | —— | 正文本身 |
| Inline Code | 无 | —— | 文本流内 |
| **Code** | 有 | **open** | lead 可视产出 · 折起用户看不到 |
| **Block Diff** | 有 | **open** | 同上 |
| Inline Diff | 无 | —— | 文本流内 |
| **Math Block** | 有 | **open** | 同上 |
| Math Inline | 无 | —— | 文本流内 |
| **Image** | 有 | **open** | 同上 |
| **Tool** | 有 | **collapsed** | 调用证据 · 默认不打扰 |
| **Thinking** | 有 | **collapsed** | 辅助思考 · 默认不打扰 |
| Reply-Quote | 无 | —— | 上下文锚点 |

**点击 `.block__bar` 的 chevron 图标切 `.is-open`**，其他图标按钮各有各的语义（copy / open / play / more），不承担折叠职责。

---

## 落地顺序建议

1. 读 `block-anatomy.md`，搭好 `.block` 外壳与状态机，做一个空的 chrome
2. 读 `text-markdown.md`，把纯文本流先跑通（含 inline code / emphasis / links / lists / tables）
3. 读 `code-and-diff.md`，接 Shiki 或 highlight.js 做代码 + diff
4. 读 `math.md`，接 KaTeX，实现 3 种界定符规范化
5. 读 `specialty-blocks.md`，做 tool / thinking / reply-quote / image 的折叠与流式状态
6. 回到 ground truth HTML（`v3-workshop.html`）做完工自检：每个块的默认展开、chip 中性、行号对齐、引号放大、折叠 bar 无悬空线

---

## 视觉真值

唯一的视觉 ground truth 是 [`v3-workshop.html`](../../design/03-message-blocks/v3-workshop.html)。当文字描述和它有冲突，以 HTML 为准——但**不要回退到 V1（Quiet）或 V2（Labeled）的设计**，它们已经被否决（见下一节）。

---

## 明确排除（选 V3 时被否决的方案 — 别改回来）

- ❌ **V1 Quiet** —— "§ code · lang · N 行"的 small-cap mono 边注 + 操作 hover 才显现。太静默，操作发现性差
- ❌ **V2 Labeled** —— italic Newsreader 卷注 + mono 文字链接。卷注标题感太强，让每块像独立章节，而不是消息流中的工件
- ❌ **代码语言 chip 用 Sienna（赭红）tint** —— 违反"一屏 Sienna ≤ 3-4 处"的预算。所有 chip 永远中性描边
- ❌ **Thinking / Tool 默认展开或 fade 半展开** —— 打扰正文阅读。只有 `.is-open` 才显示 body
- ❌ **Reply-quote 用圆角容器 + 左彩色竖条** —— AI-slop 反 pattern。V3 用薄 rule 线 + 放大引号，无框无竖条
- ❌ **行号与代码对不齐** —— gutter 和 body 必须相同 `font-size` + `line-height`（13px / 1.72）或使用 per-line grid
