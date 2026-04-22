# 打字指示器 · Typing Indicator

> ink dots（墨滴洇开）+ streaming cursor（流式光标）
> 视觉真值：`v-merged.html` · `.typing` / `.streaming-cursor`

---

## 1 · 结构

### 1.1 Ink Dots 打字指示器

当某个 agent（lead 或 teammate）正在生成回复但尚未产出任何文本 token 时，显示 ink dots：

```html
<div class="typing" data-agent="research">
  <span class="typing__avatar avatar--team"><em>R</em></span>
  <div class="typing__col">
    <div class="typing__head">
      <span class="typing__name">Research</span>
      <span class="msg__dot">·</span>
      <span>正在输入</span>
    </div>
    <div class="typing__indicator">
      <span class="typing__dot"></span>
      <span class="typing__dot"></span>
      <span class="typing__dot"></span>
    </div>
  </div>
</div>
```

### 1.2 Streaming Cursor

当 agent 已开始产出文本 token（SSE 流进行中），在文本末尾显示闪烁光标：

```html
<p>好的，我把这个任务拆分一下。<span class="streaming-cursor"></span></p>
```

---

## 2 · CSS

### 2.1 Ink Dots

```css
.typing {
  display: grid;
  grid-template-columns: 52px 1fr;
  gap: var(--space-4);
  padding: var(--space-4) 0;
  border-top: 1px dashed var(--rule-soft);
}

.typing__indicator {
  display: inline-flex;
  align-items: center;
  gap: 5px;
  padding: 10px 16px;
  background: var(--surface-raised);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
}

.typing__dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--content-muted);
  animation: ink-swell 1.4s ease-in-out infinite;
}
.typing__dot:nth-child(2) { animation-delay: 200ms; }
.typing__dot:nth-child(3) { animation-delay: 400ms; }

@keyframes ink-swell {
  0%   { opacity: .25; transform: scale(.7); }
  40%  { opacity: 1;   transform: scale(1.1); }
  60%  { opacity: 1;   transform: scale(1); }
  100% { opacity: .25; transform: scale(.7); }
}
```

### 2.2 Streaming Cursor

```css
.streaming-cursor {
  display: inline-block;
  width: 2px;
  height: 1em;
  background: var(--content);
  margin-left: 2px;
  vertical-align: text-bottom;
  animation: cursor-blink 1s step-end infinite;
}

@keyframes cursor-blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0; }
}
```

---

## 3 · 状态机

打字指示器与流式光标是**互斥**的——同一 agent 在同一时刻只能处于其中一种状态：

| 阶段 | 显示 | 触发 |
|---|---|---|
| `idle` | 无 | agent 无任务 |
| `thinking` | ink dots | SSE 流开始，首个 token 尚未到达 |
| `streaming` | streaming cursor | 首个文本 token 到达，ink dots 移除 |
| `done` | 无 | SSE 流结束，cursor 移除 |

### 3.1 状态切换逻辑

```js
function onSSEStart(agentKey) {
  // 如果该 agent 还没有消息行，插入 typing indicator
  insertTypingIndicator(agentKey);
}

function onFirstToken(agentKey) {
  // 移除 typing indicator，创建消息行，追加 streaming cursor
  removeTypingIndicator(agentKey);
  createMessageRow(agentKey);
}

function onToken(agentKey, token) {
  // 追加文本到消息体，cursor 自动在末尾
  appendToken(agentKey, token);
  maybeStickToBottom(scrollEl);
}

function onSSEEnd(agentKey) {
  // 移除 cursor
  removeStreamingCursor(agentKey);
}
```

---

## 4 · 多 Agent 同时打字

当多个 agent 同时处于 `thinking` 状态时：

- **每个 agent 独立显示自己的 typing indicator 行**——不做合并
- 排列顺序：lead 永远在最前，teammates 按 `data-agent` 字母序
- 最多同时显示 **3 个** typing indicator；超过时，第 4+ 个合并为 `+N more...` 文本

```html
<!-- 3 个 agent 同时打字 -->
<div class="typing" data-agent="lead">...</div>
<div class="typing" data-agent="coder">...</div>
<div class="typing" data-agent="research">...</div>
```

```html
<!-- 4+ 个 agent 同时打字 -->
<div class="typing" data-agent="lead">...</div>
<div class="typing" data-agent="coder">...</div>
<div class="typing" data-agent="research">...</div>
<div class="typing-overflow">
  <span class="typing-overflow__text">+2 more typing...</span>
</div>
```

```css
.typing-overflow {
  padding: var(--space-2) 0;
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  color: var(--content-soft);
  font-style: italic;
}
```

---

## 5 · Avatar 复用

`.typing__avatar` 的样式与 `.msg` 中的 `.avatar` 完全一致（52px 圆形 + 衬线斜体字母），使用相同的变体 class：

| 角色 | class | 效果 |
|---|---|---|
| Lead | `.avatar--lead` | Sienna-tint 背景 |
| Teammate | `.avatar--team` | Moss-tint 背景 |
| User | 默认 | surface-sunken 背景 |

---

## 6 · 性能约束

- `ink-swell` 动画使用 `transform` + `opacity`，**不触发 layout/paint**
- `cursor-blink` 使用 `opacity`，同上
- typing indicator 的 DOM 节点在 `done` 状态后**必须移除**，不用 `display: none` 隐藏
- 同时存在的 typing indicator ≤ 3 个 DOM 节点 + 1 个 overflow 文本

---

## 7 · 无障碍

- `.typing__indicator` 需要 `aria-label="{agent name} 正在输入"` 
- `.streaming-cursor` 需要 `aria-hidden="true"` —— 纯视觉装饰
- `prefers-reduced-motion: reduce` 下：
  - `ink-swell` → 静态显示三个点（opacity: 1, scale: 1）
  - `cursor-blink` → 静态显示光标（opacity: 1）

```css
@media (prefers-reduced-motion: reduce) {
  .typing__dot {
    animation: none;
    opacity: 1;
    transform: scale(1);
  }
  .streaming-cursor {
    animation: none;
  }
}
```

---

## 8 · 禁止事项

- ❌ 使用 `scrollIntoView` 让 typing indicator 进入视口
- ❌ 在 ink dots 上加渐变背景或彩色光晕
- ❌ typing indicator 使用骨架屏 / shimmer / placeholder 行
- ❌ streaming cursor 宽度 > 3px（太粗像竖线，不像光标）
- ❌ 多 agent 打字时合并成单行 "3 agents typing..."（失去群聊感）
