# Orangutan 聊天动态 · Step 04

> 本目录是 **Step 04 · 聊天动态** 的最终落地提示词。
> 任何实现 Orangutan 前端聊天交互层（打字指示、表情贴图、teammate 入场）的开发者或 agent，应先阅读本目录，再动手。
> 视觉真值 ground truth：[`web/design/04-chat-dynamics/v-merged.html`](../../design/04-chat-dynamics/v-merged.html)

---

## 一句话气质

**"墨滴洇开 · 边栏批注 · 卡片浮现"** —— 打字是纸上的墨滴，表情是手稿的边栏批注，teammate 的到来是卡片从虚无中浮现。

---

## 三条铁律

1. **打字指示器只用 ink dots（墨滴洇开），不用波浪、骨架屏、或 SVG 笔画。** 墨滴的 swell/fade 动画是手稿气质的延伸——纸面上墨迹的洇开。
2. **表情贴图只用 margin annotation（边栏批注），不用胶囊徽章、不用高亮底色。** 左侧 2px rule-soft 竖线 + 纯文字，像手稿页边的铅笔批注。
3. **Teammate 入场只用 pop-in（中心缩放），不用从左滑入。** 卡片从 92% 缩放到 100% + 淡入，像便签被贴上。

---

## 文件索引

| 文件 | 用途 |
|---|---|
| [`typing-indicator.md`](./typing-indicator.md) | 打字指示器（ink dots + streaming cursor）的结构、CSS、动画时序、状态机 |
| [`reactions.md`](./reactions.md) | 表情贴图（margin annotation 风格）的结构、交互、picker 弹层、数据模型 |
| [`teammate-entry.md`](./teammate-entry.md) | 左栏 teammate 卡片入场动画、状态指示器（dot + progress bar）、状态枚举 |
| [`guardrails.md`](./guardrails.md) | 本步骤的禁止事项、性能约束、无障碍要求、自检表 |

---

## 依赖与边界

### 依赖
- [`web/prompt/01-visual-language/`](../01-visual-language/) —— 所有 token、字体、颜色、阴影
- [`web/prompt/02-layout/`](../02-layout/) —— 消息容器 `.msg`、左栏 `.team-card`、chat-scroll
- [`web/prompt/03-message-blocks/`](../03-message-blocks/) —— `.msg__body` 内的块级渲染

### 本步骤范围
- 打字指示器（多 agent 同时打字时的显示策略）
- 流式文本光标
- 表情贴图（添加 / 显示 / 移除 / picker）
- Teammate 卡片入场动画
- Teammate 状态指示器（dot + progress bar）

### 不在本步骤
- 附件面板（上传文件 / URL / workspace 文件） → **Step 05**
- 右栏 teammate 详情时间线 → **Step E**
- 主题切换圆圈掠过动画 → **Step 07**
- 顶栏配置页面 → **Step 06**
- Orangutan 动态 SVG → **Step 08**

---

## 落地顺序建议

1. 读 `typing-indicator.md`，实现 ink dots + streaming cursor
2. 读 `reactions.md`，实现 margin annotation 反应 + picker
3. 读 `teammate-entry.md`，实现 pop-in 动画 + 状态指示器
4. 用 `guardrails.md` 自检表逐条核验
5. 回到 ground truth HTML 做视觉对齐

---

## 视觉真值

唯一的视觉 ground truth 是 `v-merged.html`。当文字描述和它有冲突，以 HTML 为准。

---

## 明确排除

- ❌ 胶囊徽章式表情（V1 的 stamp 风格）—— 被否决，太像 Slack/Discord
- ❌ SVG 笔画式打字指示器（V2 的 quill stroke）—— 被否决，不够直观
- ❌ 从左滑入的 teammate 入场（V1 的 slide-in）—— 被否决，与 pop-in 比缺"浮现"感
- ❌ 打字指示器用骨架屏 / shimmer —— AI-slop，与手稿气质完全冲突
