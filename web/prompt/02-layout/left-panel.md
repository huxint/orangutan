# 左栏 · Contributors（卡片式）

> 260px 默认 · 可拖拽 220–360 · 卡片含头像 / 名字 / 活动预览 / 指标
> 视觉真值：`v1-column.html` · `.left-panel`
>
> **这是 V1 + V3 混搭版**：骨架是 V1（对称三栏 · push 右栏），但左栏用 V3 的卡片以承载"详细看得到在干嘛"。

---

## 1 · 骨架

```html
<aside class="left-panel" id="left-panel">
  <div class="panel-header">
    <h3 class="panel-header__title"><em>Contributors</em></h3>
    <span class="panel-header__sub">4 · 2 active</span>
  </div>

  <button class="team-card" type="button" data-agent-key="default" aria-current="false">
    <div class="team-card__head">
      <span class="team-avatar lead" aria-hidden="true">
        <em>L</em>
        <span class="team-avatar__status status-work"></span>
      </span>
      <span class="team-card__body">
        <span class="team-card__name">
          lead
          <span class="team-card__role">主协调</span>
        </span>
        <span class="team-card__meta">思考中 · 3 tools · 4s</span>
      </span>
      <span class="team-card__arrow" aria-hidden="true">›</span>
    </div>
    <div class="team-card__activity">
      读取 <em>agent-loop.hpp</em> 第 142 行；识别 iteration-cost trap；等 research 回传 TTL 文档。
    </div>
    <div class="team-card__foot">
      <span>tokens 1,147</span>
      <span class="dot" aria-hidden="true">·</span>
      <span>cache hit 82%</span>
    </div>
  </button>

  <!-- 重复更多 .team-card -->

  <button class="add-card" type="button">+ spawn new teammate</button>

  <div class="panel-hint">
    <span class="panel-hint__tag">Hint</span>
    点击 teammate 展开右侧详情 · lead 会在本窗显示其思考 / 工具调用全过程
  </div>
</aside>
```

```css
.left-panel {
  overflow-y: auto;
  padding: var(--space-4);
  background: var(--surface);
  min-width: var(--left-w-min);
}
```

---

## 2 · Panel Header

```css
.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
  padding: 0 var(--space-1) var(--space-3);
  border-bottom: 1px solid var(--rule-soft);
  margin-bottom: var(--space-4);
}
.panel-header__title {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 18px;
  letter-spacing: -.01em;
  margin: 0;
}
.panel-header__sub {
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--content-muted);
  letter-spacing: .04em;
}
```

**文案规则**：
- 标题永远 `Contributors`（英文 · 斜体）—— 锚定手稿气质
- 副标 `{total} · {active} active` 例如 `4 · 2 active`；`active` 指非 idle 状态数

**必须用 `<h3>`**（不是 `<div>`）— 语义保留。

---

## 3 · Team Card

### 3.1 外壳

用 `<button>` 而不是 `<div>`：整卡成为单一 focusable，整卡点击。

```css
.team-card {
  background: var(--surface);
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  padding: var(--space-3);
  margin-bottom: var(--space-3);
  cursor: pointer;
  transition:
    border-color var(--duration-fast) var(--ease),
    background var(--duration-fast) var(--ease);
  width: 100%;
  text-align: left;
  border: 1px solid var(--rule-soft);
  font-family: inherit;
}
.team-card:hover {
  border-color: var(--rule);
  background: var(--surface-raised);
}
.team-card[aria-current="true"] {
  background: var(--surface-raised);
  border-color: var(--rule);
  box-shadow: var(--shadow-1);
}
.team-card:focus-visible {
  outline: none;
  box-shadow: 0 0 0 2px var(--surface), 0 0 0 4px oklch(from var(--signal) l c h / .3);
}
```

### 3.2 Head（36px avatar + 名字 + meta + 箭头）

```css
.team-card__head {
  display: grid;
  grid-template-columns: 36px 1fr auto;
  gap: var(--space-3);
  align-items: center;
  margin-bottom: var(--space-2);
}
```

#### Avatar

```css
.team-avatar {
  width: 36px; height: 36px;
  border-radius: 50%;
  background: var(--surface-sunken);
  display: flex;
  align-items: center;
  justify-content: center;
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 16px;
  color: var(--content);
  box-shadow: inset 0 0 0 1px oklch(22% 0.035 255 / .12);
  flex-shrink: 0;
  position: relative;
}
```

#### Avatar 配色（按 agent 类型 · 稳定映射）

```css
.team-avatar.lead  { background: var(--signal-tint);   color: var(--signal-deep);   box-shadow: inset 0 0 0 1px oklch(55% 0.13 42 / .35); }
.team-avatar.moss  { background: var(--accent-tint);   color: var(--accent-deep);   box-shadow: inset 0 0 0 1px oklch(44% 0.08 150 / .35); }
.team-avatar.bark  { background: oklch(82% 0.05 60);   color: oklch(34% 0.06 55);   box-shadow: inset 0 0 0 1px oklch(55% 0.08 55 / .35); }
.team-avatar.slate { background: oklch(88% 0.035 230); color: oklch(34% 0.08 240); box-shadow: inset 0 0 0 1px oklch(50% 0.08 235 / .35); }

[data-theme="dark"] .team-avatar.bark  { background: oklch(32% 0.05 55);  color: oklch(82% 0.08 60); }
[data-theme="dark"] .team-avatar.slate { background: oklch(30% 0.04 240); color: oklch(82% 0.1 235); }
```

**稳定映射**（同一 agent_key 每次进入颜色一致）：
```js
function avatarVariant(agentKey) {
  if (agentKey === 'default' || agentKey === 'lead') return 'lead';
  const palettes = ['moss', 'bark', 'slate'];
  let h = 0;
  for (const c of agentKey) h = (h * 31 + c.charCodeAt(0)) | 0;
  return palettes[Math.abs(h) % palettes.length];
}
```

#### 状态指示器（头像右下角小圆点）

```css
.team-avatar__status {
  position: absolute;
  bottom: -1px; right: -1px;
  width: 11px; height: 11px;
  border-radius: 50%;
  border: 2px solid var(--surface);
}
.status-idle  { background: var(--content-soft); }
.status-work  { background: var(--accent);  animation: pulse 1.6s ease-in-out infinite; }
.status-wait  { background: var(--signal);  }
.status-error { background: var(--signal);  animation: flash 0.9s step-end infinite; }

@keyframes pulse {
  0%, 100% { opacity: 1; transform: scale(1); }
  50%      { opacity: .65; transform: scale(0.85); }
}
@keyframes flash {
  0%, 50% { opacity: 1; }
  51%, 100% { opacity: 0.3; }
}
```

状态语义：

| 状态 | 触发 | 视觉 |
|---|---|---|
| `idle` | agent 未在活跃 session | 灰点 · 静 |
| `work` | 有 in-flight tool_start 无 tool_end | 绿点 · pulse |
| `wait` | 有 pending_approval | 红点 · 静 |
| `error` | 最近一次 tool_end `is_error = true` | 红点 · flash |

从 `/api/v1/events` 订阅 `chat.tool_start` / `chat.tool_end` / `approval_request` 推导。

#### 名字 + role

```css
.team-card__body { min-width: 0; }
.team-card__name {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 16px;
  color: var(--content);
  letter-spacing: 0;
  display: flex;
  align-items: baseline;
  gap: 6px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.team-card__role {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 400;
  font-size: 12px;
  color: var(--signal-deep);
}
```

**role 文案**：
- lead agent → `主协调`
- 经 `spawn_worker` 创建的 teammate → `委派`
- 手动添加的 external agent（罕见） → `外协`

#### Meta（mono 小字）

```css
.team-card__meta {
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--content-muted);
  letter-spacing: .02em;
  margin-top: 2px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
```

格式：`{状态关键字} · {进行中 tools 数} · {elapsed}`

| 状态 | meta 示例 |
|---|---|
| work | `思考中 · 3 tools · 4s` |
| wait | `等待审批 · edit_file` |
| idle | `空闲 · 就绪` |
| error | `错误 · read_file 失败` |

---

### 3.3 Activity Preview（2 行斜体）

```css
.team-card__activity {
  font-family: var(--font-body);
  font-size: 13px;
  font-style: italic;
  color: var(--content-muted);
  line-height: 1.5;
  padding-top: var(--space-2);
  border-top: 1px dashed var(--rule-soft);
  margin-top: var(--space-2);
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}
.team-card__activity em {
  color: var(--content);
  font-style: normal;
  font-family: var(--font-mono);
  font-size: 11px;
  background: var(--surface-sunken);
  padding: 1px 4px;
  border-radius: var(--radius-sm);
  margin: 0 2px;
}
```

**内容来源**：
- lead 的 activity：lead 的最近 2-3 个 tool_start 文件名 / 关键词 · 以自然语句组织
  - 例：`读取 <em>agent-loop.hpp</em>；识别 cost trap；等 research 返回。`
- teammate 的 activity：teammate 的 tool 流摘要
  - 例：`检索 <em>prompt caching</em>；抓取 <em>docs.anthropic.com</em>；整理规则。`

**`<em>` 内包文件名 / API 名 / 关键词** —— 自动被 mono 样式化成"行内代码标签"。

**硬约束**：永远 2 行上限（`-webkit-line-clamp: 2`）——超长 ellipsis。

`idle` 状态的 teammate **不显示** activity 区域（整个 `.team-card__activity` 不渲染）。

---

### 3.4 Foot（指标行）

```css
.team-card__foot {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding-top: var(--space-2);
  margin-top: var(--space-2);
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--content-soft);
  letter-spacing: .02em;
}
.team-card__foot .dot { color: var(--rule); }
```

**内容**：
- 左：tokens 使用 · 例 `tokens 1,284`
- 右：cache 命中率 · 例 `cache hit 71%` · 若无数据隐藏
- 中间分隔：`·`

**等待状态**的 foot 可换成 `paused 12s · 审批队列 · 1`。

`idle` 的 teammate **不显示** foot。

---

## 4 · 新增按钮（add-card）

```html
<button class="add-card" type="button" aria-label="由 lead 唤起新 teammate">
  + spawn new teammate
</button>
```
```css
.add-card {
  display: block;
  width: 100%;
  background: transparent;
  border: 1px dashed var(--rule);
  border-radius: var(--radius-md);
  padding: var(--space-4);
  margin-bottom: var(--space-3);
  text-align: center;
  font-family: var(--font-body);
  font-size: 14px;
  font-style: italic;
  color: var(--content-muted);
  cursor: pointer;
  transition:
    all var(--duration-fast) var(--ease);
}
.add-card:hover {
  border-color: var(--content);
  color: var(--content);
  border-style: solid;
}
```

**行为**：点击打开"让 lead 调度新 teammate"的提示卡（Step D 细化）。**不直接 spawn** —— orchestration 权责在 lead agent 手里。

---

## 5 · Panel Hint

首次访问 / 只有 lead 时显示：

```css
.panel-hint {
  margin-top: var(--space-4);
  padding: var(--space-3);
  background: var(--surface-raised);
  border-radius: var(--radius-md);
  font-family: var(--font-body);
  font-size: var(--text-ui);
  font-style: italic;
  color: var(--content-muted);
  line-height: 1.55;
}
.panel-hint__tag {
  font-family: var(--font-mono);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: .08em;
  color: var(--content-soft);
  font-style: normal;
  display: block;
  margin-bottom: var(--space-1);
}
```

当 teammate 数 ≥ 2 可隐藏。存 `orangutan.ui.team-hint-dismissed` 本地即可。

---

## 6 · 动态新增 teammate 的进入动效

基础设施（Step D 填动画细节）：

```css
.team-card {
  animation: slide-in 0.28s var(--ease) both;
}
@keyframes slide-in {
  from { opacity: 0; transform: translateX(-8px); }
  to   { opacity: 1; transform: translateX(0); }
}
@media (prefers-reduced-motion: reduce) {
  .team-card { animation: none; }
}
```

初始渲染的 cards 不需要 stagger（一次性 mount）；动态追加的由 JS 加临时类（Step D 实现）。

---

## 7 · teammate 多到超过视口

当 team 数 > 6 时：
- 左栏自动滚动（`overflow-y: auto` 已在 §1 就位）
- **不**折叠 cards · 但可选：将 idle teammate 的 activity 区域收起（Step I 可选优化）

---

## 8 · 数据绑定

### 8.1 初始列表

```
GET /api/v1/agents         → effective agents 列表
GET /api/v1/agents/graph   → live_sessions 判断 work / idle
```

把 API 返回的 agents 映射成 cards。Filter：只显示当前 session 或全局的 agents（由 session context 决定）。

### 8.2 状态实时更新

```js
const eventSource = new EventSource('/api/v1/events');

eventSource.addEventListener('chat.tool_start', (e) => {
  const d = JSON.parse(e.data);
  updateCardStatus(d.agent_key, 'work');
  incrementActiveTools(d.agent_key, d.tool);
  appendActivity(d.agent_key, d.tool);    // 用于 activity preview
});

eventSource.addEventListener('chat.tool_end', (e) => {
  const d = JSON.parse(e.data);
  decrementActiveTools(d.agent_key);
  if (d.is_error) {
    updateCardStatus(d.agent_key, 'error');
  }
});

eventSource.addEventListener('chat.done', (e) => {
  const d = JSON.parse(e.data);
  setCardStatus(d.agent_key, 'idle');
});

eventSource.addEventListener('approval_request', (e) => {
  const d = JSON.parse(e.data);
  updateCardStatus(d.agent_key, 'wait');
});
```

### 8.3 动态追加

当主 agent 调用 `spawn_worker(worker_agent_key)` 工具：
- 监听 lead 的 `chat.tool_start` · `tool === 'spawn_worker'`
- 从 input 提取 `agent_key`，调用 `addTeamCard(agent_key)` 插入新卡片末尾
- 卡片触发 slide-in 进入动画

---

## 9 · Empty state

当只有 lead 时：
- 显示 lead 的卡片
- 下方 `.add-card` 按钮
- 下方 Panel Hint 说明"你可以让 lead 调度 research / coder / writer 等 teammate"

---

## 10 · 禁止事项

- ❌ 用 emoji 代替 status / role 文字
- ❌ 给每个 teammate 换不同 font family（都是 Newsreader）
- ❌ 在卡片上加"三点菜单"或右键菜单 —— 详情都去右栏
- ❌ 同一 agent_key 两次渲染颜色不同
- ❌ activity preview 超 2 行
- ❌ 超过 10 个 teammate 不分组（Step I 才加折叠）
- ❌ 把卡片做得非常窄（左栏最小 220px 是为了卡片呼吸）

---

## 11 · 自检

- [ ] 点击任意 team-card 打开右栏 · 该卡 `aria-current="true"`
- [ ] 不同 agent_key 的头像颜色稳定（刷新不变）
- [ ] 状态 pulse / flash 动画在 `prefers-reduced-motion` 下被禁用
- [ ] Empty state（仅 lead）显示 Panel Hint
- [ ] team-card Focus 后 Enter / Space 可触发（button 天然支持）
- [ ] team-card meta 永远单行 · activity 永远 ≤ 2 行
- [ ] 新增 teammate 时有 slide-in 进入动效
- [ ] idle teammate 不显示 activity 与 foot 区域
