# 右栏 · Teammate Detail（Push Drawer）

> 320px · push（挤中栏）· 默认 closed · 展开时显示激活 teammate 的活动详情、指标、思考
> 视觉真值：`v1-column.html` · `.right-panel`

---

## 1 · 骨架

```html
<aside class="right-panel" id="right-panel" aria-hidden="true" tabindex="-1">
  <div class="right-panel__inner">
    <header class="detail-head">
      <h3 class="detail-head__title">
        <em>teammate · research</em>
        活动详情
      </h3>
      <button class="detail-close" type="button" aria-label="关闭详情">×</button>
    </header>

    <div class="detail-metrics">
      <div class="metric">
        <div class="metric__label">Tokens</div>
        <div class="metric__value">1,284</div>
      </div>
      <div class="metric">
        <div class="metric__label">Elapsed</div>
        <div class="metric__value">4.2s</div>
      </div>
    </div>

    <div class="detail-section">
      <h4 class="detail-section-title">§ Tool Calls · 5 total</h4>
      <!-- .activity 时间线 -->
    </div>

    <div class="detail-section">
      <h4 class="detail-section-title">§ Thinking</h4>
      <!-- thinking 片段 -->
    </div>
  </div>
</aside>
```

```css
.right-panel {
  overflow-y: auto;
  background: var(--surface-raised);
  border-left: 1px solid var(--rule-soft);
  min-width: 0;
  opacity: 0;
  transition: opacity 240ms var(--ease);
}
[data-right="open"] .right-panel {
  opacity: 1;
}
.right-panel__inner {
  padding: var(--space-5) var(--space-4);
}
```

**必守**：
- 默认 `aria-hidden="true"` + `tabindex="-1"`（不可 tab 到）
- 打开后 `aria-hidden="false"`，焦点移入面板（延迟 280ms 等 grid 过渡完成）
- 关闭后焦点回到激活的 `.team-card`

---

## 2 · Detail Head

```css
.detail-head {
  display: flex;
  justify-content: space-between;
  align-items: start;
  gap: var(--space-3);
  padding-bottom: var(--space-3);
  margin-bottom: var(--space-4);
  border-bottom: 1px solid var(--rule-soft);
}
.detail-head__title {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 22px;
  margin: 0;
  line-height: 1.15;
}
.detail-head__title em {
  color: var(--accent-deep);
  font-style: normal;
  font-weight: 400;
  font-family: var(--font-mono);
  font-size: 13px;
  letter-spacing: .02em;
  display: block;
  margin-bottom: 2px;
}
.detail-close {
  width: 28px; height: 28px;
  border-radius: var(--radius-sm);
  background: transparent;
  border: none;
  color: var(--content-muted);
  cursor: pointer;
  font-size: 16px;
  line-height: 1;
  flex-shrink: 0;
}
.detail-close:hover {
  background: var(--surface);
  color: var(--content);
}
.detail-close:focus-visible {
  outline: none;
  box-shadow: 0 0 0 2px var(--surface-raised), 0 0 0 4px oklch(from var(--signal) l c h / .3);
}
```

**标题结构**：
- 上方 `em`：mono 小字 · 显示 agent_key 及关系 · 例 `teammate · research` / `主协调 · lead`
- 下方 大字：描述性标题 · 例 `活动详情` / `思考与工具`

---

## 3 · Metrics Grid

```css
.detail-metrics {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: var(--space-3);
  margin-bottom: var(--space-5);
}
.metric {
  padding: var(--space-3);
  background: var(--surface);
  border-radius: var(--radius-md);
}
.metric__label {
  font-family: var(--font-mono);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: .08em;
  color: var(--content-muted);
  margin-bottom: 2px;
}
.metric__value {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 24px;
  color: var(--content);
  line-height: 1;
}
```

**核心指标**（前两个永远显示）：
- **Tokens**：累计消耗
- **Elapsed**：累计耗时

**可扩展指标**（通过 2+2 或 2x3 grid）：
- Cache hit %
- Tool calls
- Avg latency
- 审批队列 · N

Grid 可从 `1fr 1fr` 扩展到 `repeat(3, 1fr)` 容纳更多指标（Step E 决定）。本步骤保持 2 格。

---

## 4 · Activity 时间线

```html
<div class="activity">
  <div class="activity__dot done" aria-hidden="true"></div>
  <div class="activity__body">
    <div class="activity__label">web_search</div>
    <div class="activity__sub">prompt caching TTL anthropic 2026</div>
  </div>
  <div class="activity__time">142 ms</div>
</div>
```

```css
.detail-section-title {
  font-family: var(--font-mono);
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: .08em;
  color: var(--content-muted);
  margin: 0 0 var(--space-3);
  font-weight: 500;
}

.activity {
  display: grid;
  grid-template-columns: 16px 1fr auto;
  gap: var(--space-3);
  padding: var(--space-3) 0;
  border-top: 1px dashed var(--rule-soft);
  font-family: var(--font-mono);
  font-size: 12px;
  align-items: baseline;
}
.activity:first-of-type { border-top: none; }
.activity__dot {
  width: 8px; height: 8px;
  border-radius: 50%;
  background: var(--accent);
  margin-top: 4px;
}
.activity__dot.done { background: var(--content-soft); }
.activity__dot.err  { background: var(--signal); }
.activity__label {
  color: var(--content);
  font-weight: 500;
}
.activity__sub {
  color: var(--content-muted);
  font-family: var(--font-body);
  font-style: italic;
  font-size: 13px;
  margin-top: 2px;
}
.activity__time {
  color: var(--content-soft);
  font-size: 10px;
}
```

**dot 状态**：
- 无 class（默认 accent 色）：运行中
- `.done`（灰）：已完成成功
- `.err`（红）：失败

**文案**：
- `activity__label`：tool 名（mono）
- `activity__sub`：tool 入参的自然语言摘要（斜体衬线）
  - `web_search` → query 字符串
  - `read_file` → 文件相对路径
  - `edit_file` → 文件 + 变更行数概览
- `activity__time`：执行耗时 / `—`（未完成）

---

## 5 · Thinking 区（仅 teammate）

lead 的 thinking 在主聊天流；teammate 的 thinking 在右栏展开：

```html
<div class="detail-section detail-section--thinking">
  <h4 class="detail-section-title">§ Thinking</h4>
  <blockquote class="think-snippet">
    <span class="think-snippet__marker">T+1.2s</span>
    <span>需要先确认官方文档 TTL 的具体单位是分钟还是小时，再决定后续 fetch 策略。</span>
  </blockquote>
  <!-- 多段 -->
</div>
```

```css
.think-snippet {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: var(--space-3);
  padding: var(--space-3);
  margin: 0 0 var(--space-3);
  background: color-mix(in oklch, var(--surface) 70%, transparent);
  border-radius: var(--radius-md);
  font-family: var(--font-body);
  font-style: italic;
  font-size: 14px;
  color: var(--content-muted);
  line-height: 1.6;
}
.think-snippet__marker {
  font-family: var(--font-mono);
  font-style: normal;
  font-size: 10px;
  letter-spacing: .1em;
  color: var(--content-soft);
  padding-top: 2px;
  white-space: nowrap;
}
```

---

## 6 · 状态机

右栏的三种内容态：

| 状态 | 触发 | 渲染 |
|---|---|---|
| `closed` | 初始 / 用户关闭 | 面板不展示（grid 宽度 0）· 内容保留不销毁 |
| `open · loading` | 刚打开 · 数据未就绪 | metrics 显示 `—` · 无 activity 行 · thinking 空 · skeleton 占位 |
| `open · loaded` | 数据已就绪 | 实数据渲染 |
| `open · empty` | 所选 teammate 从未工作过 | 显示"{teammate} 还未开始工作" 空态 |
| `open · error` | 数据加载失败 | 显示错误 + 重试按钮 |

---

## 7 · 数据绑定

### 7.1 打开右栏时

```js
function renderTeammateDetail(agentKey) {
  const panel = document.getElementById('right-panel');
  panel.setAttribute('data-agent', agentKey);

  updateDetailHead(agentKey);                // 名称 / role
  showDetailLoading();

  const data = collectTeammateActivity(agentKey);
  if (data.empty) {
    showDetailEmpty(agentKey);
  } else {
    renderDetailMetrics(data.metrics);
    renderDetailActivities(data.tools);
    renderDetailThinking(data.thinking);
  }
}
```

### 7.2 数据来源（当前后端约束）

**重要**：当前后端的 `/api/v1/chat` SSE 流**只发出主 agent 的事件流**——teammate 的 tool 调用出现在主 agent 的 `tool_start` 里（当 tool_name 是 `spawn_worker` / `send_message`）。

因此 teammate 活动的**解析策略**：

```js
// 从主 chat SSE 的事件历史中筛选属于某个 teammate 的活动
function collectTeammateActivity(agentKey) {
  const events = getChatEventHistory(currentSessionId);

  // 找到第一个 spawn_worker({agent_key: agentKey}) 事件 · 作为 teammate session 起点
  const spawn = events.find(e =>
    e.type === 'tool_start' && e.tool === 'spawn_worker' && e.input.agent_key === agentKey
  );
  if (!spawn) return { empty: true };

  // 截取从 spawn 到对应 tool_end 之间的事件
  // （orchestration runtime 通常在 tool_end 时返回 worker 的完整 history）
  const end = events.find(e =>
    e.type === 'tool_end' && e.id === spawn.id
  );

  // 从 tool_end.content 里解析 worker 的 tool 序列（JSON / markdown）
  const activities = parseWorkerActivity(end?.content);

  return {
    empty: activities.length === 0,
    metrics: {
      tokens: activities.reduce((s, a) => s + (a.tokens || 0), 0),
      elapsed: activities.reduce((s, a) => s + (a.elapsed_ms || 0), 0) / 1000,
    },
    tools: activities,
    thinking: extractThinking(end?.content),
  };
}
```

**TODO（后端协作）**：orchestration runtime 应为 spawn 的 worker agent **独立向 event bus 发布 `chat.*` 事件**（用 worker 的 agent_key + 合成 session_id），以便前端直接订阅，不再从主 agent 的 tool_end content 里解析。把这个 gap 标记为**已知技术债**，在 Step E 细化时对齐后端。

### 7.3 实时更新

只要右栏是 `open` 且 agentKey 与某个 in-flight tool 相关，应实时追加 activity 行：

```js
eventSource.addEventListener('chat.tool_start', (e) => {
  const d = JSON.parse(e.data);
  if (panel.getAttribute('data-agent') === d.agent_key ||
      isSubToolOf(d, panel.getAttribute('data-agent'))) {
    appendActivityRow(d);
  }
});
```

---

## 8 · Empty States

### 8.1 Teammate 未工作过

```html
<div class="detail-empty">
  <div class="detail-empty__glyph">—</div>
  <p class="detail-empty__title"><em>research</em> 还未开始工作</p>
  <p class="detail-empty__sub">
    对 lead 说：<br>
    &ldquo;让 research 去查一下…&rdquo;
  </p>
</div>
```

```css
.detail-empty {
  text-align: center;
  padding: var(--space-7) var(--space-4);
  color: var(--content-muted);
}
.detail-empty__glyph {
  font-family: var(--font-display);
  font-style: italic;
  font-size: 48px;
  color: var(--content-soft);
  margin-bottom: var(--space-3);
}
.detail-empty__title {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 18px;
  color: var(--content);
  margin: 0 0 var(--space-2);
}
.detail-empty__sub {
  font-family: var(--font-body);
  font-style: italic;
  font-size: 14px;
  line-height: 1.6;
  margin: 0;
}
```

### 8.2 右栏打开但无选中 teammate

理论不应发生（打开右栏必带 teammate），但防御性实现：显示 "Choose a contributor" 空态。

---

## 9 · 禁止事项

- ❌ 把右栏做成 overlay（V2 风格 · V1 必须 push）
- ❌ 在右栏做输入 / chat · 右栏是**只读视图**
- ❌ 显示 lead 的 thinking（lead thinking 在主聊天区；右栏只给 teammate）
- ❌ 让右栏侵占中栏的 min-width（永远 ≥ 280 且不挤碎中栏）
- ❌ 使用左侧强调色竖条分隔 section（用 title 标题 + 虚线分隔即可）

---

## 10 · 自检

- [ ] `aria-hidden` 在关闭时为 `true`，打开时为 `false`
- [ ] 打开右栏后焦点进入面板（延迟 280ms）
- [ ] 关闭右栏焦点回激活的 team-card
- [ ] Esc 键关闭右栏
- [ ] 空态（teammate 未工作）正确显示
- [ ] 数据加载失败时有重试 UI
- [ ] activity 行的 dot 颜色正确反映状态
- [ ] thinking 片段以斜体衬线呈现 · 不与主聊天冲突
- [ ] dark 主题下所有 metric / activity 对比度达标
