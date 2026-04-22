# Teammate 入场与状态 · Entry & Status

> pop-in 动画 + status dot + progress bar
> 视觉真值：`v-merged.html` · `.team-card--entering` / `.team-card__progress`

---

## 1 · 入场动画

### 1.1 Pop-in（中心缩放浮现）

当主 agent 创建新 teammate 时，左栏对应卡片以 pop-in 动画出现：

```css
.team-card--entering {
  animation: card-pop-in 320ms var(--ease) both;
}

@keyframes card-pop-in {
  from {
    opacity: 0;
    transform: scale(.92);
  }
  to {
    opacity: 1;
    transform: scale(1);
  }
}
```

**为何选 pop-in 而非 slide-in**：
- pop-in 的"浮现"感与"便签被贴上"的手稿隐喻一致
- slide-in 暗示"从外面进来"，与左栏作为"已有成员列表"的定位矛盾
- pop-in 不依赖方向（左栏可能被压窄），更稳健

### 1.2 触发逻辑

```js
function onTeammateCreated(event) {
  const card = createTeamCard(event.teammate);
  leftPanel.appendChild(card);
  
  // 强制 reflow 以确保动画触发
  void card.offsetWidth;
  card.classList.add('team-card--entering');
  
  // 动画结束后移除 class（避免后续重排触发重复动画）
  card.addEventListener('animationend', () => {
    card.classList.remove('team-card--entering');
  }, { once: true });
}
```

### 1.3 退场动画

teammate 被移除时，反向 pop-out：

```css
.team-card--leaving {
  animation: card-pop-out 240ms var(--ease) both;
}

@keyframes card-pop-out {
  from {
    opacity: 1;
    transform: scale(1);
  }
  to {
    opacity: 0;
    transform: scale(.92);
  }
}
```

退场完成后 **移除 DOM 节点**：

```js
function onTeammateRemoved(teammateKey) {
  const card = findCard(teammateKey);
  card.classList.add('team-card--leaving');
  card.addEventListener('animationend', () => {
    card.remove();
  }, { once: true });
}
```

---

## 2 · 状态指示器

每个 teammate 卡片有两层状态指示：**status dot**（点）+ **progress bar**（进度条）。

### 2.1 Status Dot

位于卡片内 status 文字左侧：

```html
<div class="team-card__status">
  <span class="status-dot status-dot--working"></span>
  回复中 · 12s
</div>
```

| 状态 | class | 颜色 | 动画 |
|---|---|---|---|
| 空闲 | `.status-dot--idle` | `--content-soft` | 无 |
| 工作中 | `.status-dot--working` | `--accent` (Moss) | pulse 1.6s |
| 等待中 | `.status-dot--waiting` | `--signal` (Sienna) | pulse 2.4s |
| 已完成 | `.status-dot--done` | `--accent-deep` | 无 |
| 错误 | `.status-dot--error` | `--signal-deep` | 无 |

```css
.status-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  flex-shrink: 0;
}

.status-dot--working {
  background: var(--accent);
  animation: pulse-dot 1.6s ease-in-out infinite;
}

.status-dot--waiting {
  background: var(--signal);
  animation: pulse-dot 2.4s ease-in-out infinite;
}

@keyframes pulse-dot {
  0%, 100% { opacity: 1; }
  50% { opacity: .35; }
}
```

### 2.2 Progress Bar

位于卡片底部，2px 高的动画条，提供"正在活动"的视觉反馈：

```html
<div class="team-card" style="position:relative; overflow:hidden;">
  <!-- ... avatar, info ... -->
  <div class="team-card__progress"></div>
</div>
```

| 状态 | class | 颜色 | 宽度 | 动画 |
|---|---|---|---|---|
| 工作中 | `.team-card__progress` | `--accent` | 60% | sweep 3s |
| 等待中 | `.team-card__progress--waiting` | `--signal` | 30% | sweep 4s |
| 已完成 | `.team-card__progress--done` | `--accent-deep` | 100% | 无 |
| 空闲 | 不渲染 | — | — | — |

```css
.team-card__progress {
  position: absolute;
  bottom: 0;
  left: 0;
  height: 2px;
  background: var(--accent);
  border-radius: 1px;
  animation: progress-sweep 3s ease-in-out infinite;
  width: 60%;
}

.team-card__progress--waiting {
  background: var(--signal);
  animation: progress-sweep 4s ease-in-out infinite;
  width: 30%;
}

.team-card__progress--done {
  background: var(--accent-deep);
  animation: none;
  width: 100%;
}

@keyframes progress-sweep {
  0%   { left: 0; }
  50%  { left: 40%; }
  100% { left: 0; }
}
```

**为何需要 progress bar**：
- status dot 只有 6px，在卡片列表中容易被忽略
- progress bar 是 2px × 60% 的持续动画，扫一眼就能感知"谁在活动"
- 与 dot 的颜色绑定一致（Moss = 工作，Sienna = 等待），双冗余增强可读性

---

## 3 · 状态文本

卡片内 status 文字格式：

| 状态 | 文本格式 | 示例 |
|---|---|---|
| 空闲 | `空闲` | 空闲 |
| 工作中 | `{动作} · {elapsed}` | 回复中 · 12s |
| 等待中 | `等待 · {原因}` | 等待 · 队列中 |
| 已完成 | `已完成 · {duration}` | 已完成 · 34s |
| 错误 | `错误 · {brief}` | 错误 · 超时 |

elapsed 实时更新（每秒 +1），使用 `requestAnimationFrame` 或 `setInterval` 1s。

---

## 4 · 状态枚举

```ts
type TeammateStatus =
  | 'idle'       // 无任务
  | 'working'    // 正在执行任务
  | 'waiting'    // 等待资源/队列/审批
  | 'done'       // 任务完成
  | 'error';     // 执行出错
```

状态转换图：

```
idle ──→ working ──→ done
  ↑         │          │
  │         ↓          │
  └──── waiting ───────┘
              │
              ↓
            error
```

---

## 5 · Lead 的特殊处理

Lead agent（主 agent）的卡片：
- Avatar 用 `.avatar--lead`（Sienna-tint），不用 Moss
- Progress bar 颜色也用 `--signal`（赭红），不用 `--accent`
- Status 文本优先级最高，永远排在卡片列表第一位
- 不显示"空闲"状态——lead 永远处于某种工作状态

```css
.team-card--lead .team-card__progress {
  background: var(--signal);
}
```

---

## 6 · 多 Teammate 的 Hue 分配

当有多个 teammate 时，每个 teammate 的 avatar 分配不同 hue，避免视觉混淆：

```js
const TEAM_HUES = [150, 180, 210, 280, 320, 40, 80, 120]; // oklch hue

function assignHue(index) {
  return TEAM_HUES[index % TEAM_HUES.length];
}
```

每个 teammate 的 avatar 使用：

```css
/* 动态生成 */
.team-card__avatar--team-hue {
  background: oklch(88% 0.038 {hue});
  color: oklch(30% 0.06 {hue});
  box-shadow: inset 0 0 0 1px oklch(44% 0.08 {hue} / .4);
}
```

**约束**：hue 永远不能是 42（Sienna 的 hue）—— Sienna 专属 lead。

---

## 7 · 性能约束

- 入场/退场动画使用 `transform` + `opacity`，不触发 layout
- `progress-sweep` 使用 `left` 属性动画——**这是唯一允许的 left 动画**，因为 2px 高度不影响 layout
- 同时存在的 progress bar ≤ 8 条
- `pulse-dot` 动画使用 `opacity`，不触发 paint
- 退场动画结束后**必须移除 DOM**，不用 `display: none` 缓存

---

## 8 · 无障碍

- `.team-card` 是 `<button>` 或有 `role="button"` + `tabindex="0"`
- `aria-current="true"` 标记选中状态
- status dot 需要 `aria-hidden="true"`（纯视觉装饰）
- status 文本需要 `aria-live="polite"` 的父容器
- `prefers-reduced-motion: reduce` 下：
  - pop-in / pop-out → 瞬时出现/消失（animation: none）
  - progress-sweep → 静态显示（animation: none）
  - pulse-dot → 静态显示（opacity: 1）

```css
@media (prefers-reduced-motion: reduce) {
  .team-card--entering,
  .team-card--leaving {
    animation: none;
  }
  .team-card__progress {
    animation: none;
  }
  .status-dot--working,
  .status-dot--waiting {
    animation: none;
    opacity: 1;
  }
}
```

---

## 9 · 禁止事项

- ❌ slide-in 入场动画（V1 被否决）
- ❌ progress bar 高度 > 2px（太粗像进度条，不像墨迹）
- ❌ teammate avatar 使用 Sienna hue（42°）—— 专属 lead
- ❌ 退场时直接移除 DOM 不加动画（用户看不到消失过程）
- ❌ lead 卡片显示"空闲"状态
- ❌ progress bar 使用 `width` 动画（触发 layout）—— 用 `left` 位移
