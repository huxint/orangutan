# Block Anatomy · 共享外壳与原子

> 每个"产出型"块（code / diff / math / tool / thinking / image）的**共用结构**。
> 此文件定义 `.block` shell、`.block__bar` 顶栏、chip / button / dot / icon 原子、`.is-open` 状态机。
> 各具体块（code、diff、math …）的 body 细节在各自文档；本文件只管 chrome 与状态。

---

## 1 · `.block` 外壳

### HTML

```html
<div class="block">
  <div class="block__bar">…身份 & 操作…</div>
  <!-- body · 形式视块类型而定 -->
</div>
```

**要求**：
- `.block` 是 `overflow: hidden` + 圆角容器，内部一切裁剪到圆角内
- bar 必须是第一个子元素
- body 紧随 bar，单个直接子元素（避免 `:has()` 选择器失效）

### CSS

```css
.block {
  margin: var(--space-4) 0;
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  background: var(--surface);
  box-shadow: var(--shadow-1);
  overflow: hidden;
}
```

**禁止**：
- ❌ 加左侧强调色竖条（AI-slop 反 pattern）
- ❌ 加大面积渐变背景
- ❌ `box-shadow` 超过 `--shadow-2`（深阴影打破"浅油墨"气质）
- ❌ hover 放大 / `transform: scale(...)`

---

## 2 · `.block__bar` 顶栏结构

### HTML 骨架

```html
<div class="block__bar">
  <div class="block__lhs">
    <!-- 身份：dot + chip + name + meta -->
    <span class="dot dot--ok" aria-hidden="true"></span>
    <span class="block__chip">lang</span>
    <span class="block__name">identifier</span>
    <span class="block__meta">meta · state · duration</span>
  </div>
  <!-- 可选：中间控件（如 diff 的 unified/split toggle） -->
  <div class="block__rhs">
    <button class="block__btn" aria-label="…">
      <svg class="icon" viewBox="0 0 24 24">…</svg>
    </button>
  </div>
</div>
```

### CSS

```css
.block__bar {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: 8px var(--space-3) 8px var(--space-4);
  background: var(--surface-raised);
  color: var(--content-muted);
  font-family: var(--font-mono);
  font-size: 12px;
  letter-spacing: .02em;
  border-bottom: 1px solid var(--rule-soft);
  min-height: 38px;
}

.block__lhs {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  min-width: 0;            /* 允许子项 ellipsis */
  flex: 1;
}

.block__name {
  color: var(--content);
  font-weight: 500;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  min-width: 0;
}

.block__meta {
  color: var(--content-soft);
  font-size: 11px;
  white-space: nowrap;
}
.block__meta .ok  { color: var(--accent-deep); }
.block__meta .err { color: var(--signal-deep); }
.block__meta em {
  font-family: var(--font-display);
  font-style: italic;
  color: var(--content-soft);
  letter-spacing: 0;
  margin: 0 6px;
}

.block__rhs {
  display: flex;
  align-items: center;
  gap: 2px;
}
```

### 折叠态的 bar · 无悬空线

当 body 是 `.think-v3` 或 `.tool-v3` 且未 `.is-open`，`.block__bar` 的 `border-bottom` 自动去掉：

```css
.block:has(> .think-v3:not(.is-open)) .block__bar,
.block:has(> .tool-v3:not(.is-open))  .block__bar {
  border-bottom: none;
}
```

**为什么用 `:has()`** —— 比 JS 切 class 更可靠（状态同步零延迟），且 2026 年 evergreen 浏览器全支持。

---

## 3 · `.block__chip` · 中性描边胶囊

**唯一样式 · 永远中性**。代码语言（`cpp` / `python` / `ts`）、工具类型（`tool`）、块类型（`diff` / `math` / `image`）全用同一条 chip 规则。

### CSS

```css
.block__chip {
  font-family: var(--font-mono);
  font-size: 11px;
  letter-spacing: .06em;
  padding: 3px 8px;
  border-radius: var(--radius-sm);
  background: var(--surface);
  color: var(--content-muted);
  border: 1px solid var(--rule);
  line-height: 1;
  white-space: nowrap;
}
```

**禁止**：
- ❌ 给不同语言配不同 hue（AI-slop 反 pattern）
- ❌ 用 `--signal-tint` / `--accent-tint` 作背景（违反 Sienna 预算，也让 chip "抢戏"）
- ❌ 去掉 border 改成 solid 背景

**如果确实需要"强调"一条 chip**（比如 err 状态工具、deprecated 语言），用 `.block__chip--err` 增加：`color: var(--signal-deep); border-color: var(--signal);` —— 但每屏 ≤ 2 处。

---

## 4 · `.block__btn` · 方形图标按钮

### CSS

```css
.block__btn {
  background: transparent;
  border: none;
  cursor: pointer;
  width: 28px;
  height: 28px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border-radius: var(--radius-sm);
  color: var(--content-muted);
  transition: color var(--duration-fast) var(--ease),
              background var(--duration-fast) var(--ease);
}
.block__btn:hover {
  color: var(--content);
  background: color-mix(in oklch, var(--rule-soft) 80%, transparent);
}
.block__btn:focus-visible {
  outline: 2px solid var(--signal);
  outline-offset: -2px;
}
.block__btn[disabled] {
  opacity: .38;
  cursor: not-allowed;
}
```

**必守**：
- **始终带 `aria-label`** —— 图标按钮无文字，a11y 依赖 label
- **28×28 px** —— 触达目标 ≥ 28px；**移动端（viewport ≤ 768px）放大到 40×40**
- **不堆叠**：`.block__rhs` 内单行排列，超过 3 个按钮则最后一个用 `more` 溢出菜单（见 §6）

---

## 5 · `.dot` · 状态圆点

6×6 px 圆点，在 bar 左端标示状态。

### CSS

```css
.dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  display: inline-block;
  flex-shrink: 0;
}
.dot--ok  { background: var(--accent); }
.dot--err { background: var(--signal); }
.dot--run {
  background: var(--accent);
  box-shadow: 0 0 0 0 var(--accent);
  animation: pulse 1.4s infinite;
}

@keyframes pulse {
  0%   { box-shadow: 0 0 0 0 oklch(from var(--accent) l c h / .4); }
  100% { box-shadow: 0 0 0 8px oklch(from var(--accent) l c h / 0); }
}

@media (prefers-reduced-motion: reduce) {
  .dot--run { animation: none; }
}
```

### 语义映射

| dot | 用于 | 语义 |
|---|---|---|
| `.dot--ok` | tool 完成、image 加载完 | 成功 / 静默进行 |
| `.dot--err` | tool 失败、image 加载错 | 失败 |
| `.dot--run` | tool 执行中、thinking 流式中、image 生成中 | 活跃 |

**绝不**用 `.dot--ok` 的 Moss 表示"警告"，也不用 `.dot--err` 的 Sienna 表示"成功"——违反 Step 01 的信号/次信号语义映射。

---

## 6 · `.icon` · 线性 SVG 图标

**图标库选择**：**Phosphor Icons**（推荐）或 **Lucide**，二选一。禁止混用。本步骤的 ground truth HTML 用的是 hand-written 近似路径（仅为 preview 价值），实现时必须接入库。

### CSS

```css
.icon {
  width: 14px;
  height: 14px;
  stroke: currentColor;
  fill: none;
  stroke-width: 1.4;
  stroke-linecap: round;
  stroke-linejoin: round;
  flex-shrink: 0;
  display: block;
}
.icon-lg { width: 16px; height: 16px; }
```

### 核心图标命名映射

| 语义 | Phosphor | Lucide | 在哪里用 |
|---|---|---|---|
| chevron 展开折叠 | `CaretDown` | `chevron-down` | thinking / tool bar 右侧 · 折叠态朝下，展开态 `rotate(180deg)` |
| 复制 | `Copy` | `copy` | code / diff / math / tool |
| 在编辑器打开 | `ArrowUpRight` | `external-link` | code / diff |
| 运行 / 执行 | `Play` | `play` | 可运行的代码（python / shell） |
| 查看原始 LaTeX | `Code` | `code` | math block |
| 下载 | `DownloadSimple` | `download` | image |
| 更多 · 溢出菜单 | `DotsThree` | `more-horizontal` | 当操作 > 3 个 |
| 刷新重跑 | `ArrowsClockwise` | `refresh-cw` | 失败的 tool / image |

**溢出菜单规则**：`.block__rhs` 内**最多 3 个直接可见按钮**；超过时，前 2 个是最常用（copy / chevron），其余归到 `more` 触发的小 popover（popover 细节不在本 step，占位 `<!-- TODO: more-menu -->` 即可）。

---

## 7 · `.is-open` 状态机 · 单一 class 控制展开

**简单直接**：每种 body（`.tool-v3` / `.think-v3` / 等）独立切换 `display: none ↔ block` 通过 `.is-open` class。

```css
.tool-v3 { display: none; /* …其他样式… */ }
.tool-v3.is-open { display: block; }

.think-v3 { display: none; /* …其他样式… */ }
.think-v3.is-open { display: block; }
```

**为什么不用 `max-height` + transition** —— 三个原因：
1. body 里可能含 KaTeX 排版或宽代码，重排后精确高度难预测，`max-height` 的过渡会抖
2. 聊天流会因为消息接连进入而不断重新布局，`max-height` 过渡叠加触发多次抖动
3. `display` 切换的瞬间感，其实更像"折子展开"——符合手稿气质，不是 drawer-slide 的工具气质

**如果真的需要过渡反馈**（用户报告 harsh），加一条 `.block` 整块的淡入：

```css
.block > *:not(.block__bar) { animation: fade-in 120ms var(--ease); }
@keyframes fade-in { from { opacity: 0 } to { opacity: 1 } }
```

仅此而已，不要用 `max-height`。

### chevron 的方向同步

```js
function toggleBody(btn, bodySelector) {
  const block = btn.closest('.block');
  const body  = block?.querySelector(bodySelector);
  if (!body) return;
  const open = body.classList.toggle('is-open');
  btn.setAttribute('aria-expanded', String(open));
  btn.querySelector('svg')?.style.setProperty(
    'transform',
    open ? 'rotate(180deg)' : 'rotate(0)'
  );
}
```

---

## 8 · 默认展开策略 · 实现表

| 块 | 初始 class | 切换入口 | 切换 API |
|---|---|---|---|
| Code | `.code-v3`（无 is-open 概念 · 总是展开） | —— | —— |
| Block Diff | `.diff-v3`（总是展开） | `.diff-v3__toggle` 切 unified/split | `diff.dataset.view = 'unified' | 'split'` |
| Math Block | `.math-block`（总是展开） | —— | —— |
| Image | `.image-v3`（总是展开） | —— | —— |
| **Tool** | `.tool-v3`（默认无 is-open） | bar 内 chevron 按钮 | `toggleBody(btn, '.tool-v3')` |
| **Thinking** | `.think-v3`（默认无 is-open） | bar 内 chevron 按钮 | `toggleBody(btn, '.think-v3')` |

**"全部展开" · 可选便捷**：在顶栏或消息 hover 时提供"展开全部 tool/thinking"一键按钮——不要常驻，偶尔用。

---

## 9 · 无障碍 · 不妥协的 a11y

- **所有图标按钮**必须 `aria-label`
- **可折叠 body 对应的 chevron 按钮**必须 `aria-expanded="true|false"` 同步
- **可折叠 body 容器**必须 `role="region"` + `aria-labelledby` 指向其 bar 内的 `.block__name`
- **状态 dot** 必须 `aria-hidden="true"`——状态意义已在 `.block__meta` 的文字中重复（色盲可读性）
- **图标按钮** `:focus-visible` 必须有 2px Sienna outline
- **键盘**：chevron 按钮 Enter / Space 切展开；在 code 块内 Tab 跳到下一块

---

## 10 · 尺寸/颜色/空间速查

| 量 | 值 | 出处 |
|---|---|---|
| bar 高度 | min-height 38px（折叠态）· 44px（body 展开态内部 padding 加高） | 可点目标 |
| bar 背景 | `--surface-raised` | Step 01 |
| body 背景 | `--surface`（比 bar 稍浅） | 视觉区分 |
| chip 字号 | 11px | mono |
| chip 字距 | .06em | mono |
| name 字号 | 12px | mono |
| meta 字号 | 11px | mono |
| icon 尺寸 | 14×14 默认 / 16×16 `.icon-lg` | 可读性 |
| icon 按钮 | 28×28 | Fitts 律下限 |
| icon 按钮 gap | 2px | 紧凑 |
| block 边框 | `1px solid var(--rule-soft)` | Step 01 |
| block 圆角 | `--radius-md` (8) | Step 01 |
| block 阴影 | `--shadow-1` | Step 01 |
| block 上下外边距 | `var(--space-4)`（16px） | 消息内块间距 |

---

## 11 · 自检（写任何块前）

- [ ] 新块以 `.block` 外壳包住，内部 `.block__bar` + body 两子元素
- [ ] bar 左侧：`dot?` + `chip?` + `name` + `meta?`，右侧：`button*`
- [ ] chip 全中性，不用 `--signal-tint` / `--accent-tint`
- [ ] icon 按钮 28×28 + `aria-label`
- [ ] dot 的颜色与 meta 文字"双重表达"（色盲可读）
- [ ] 折叠态 body 是 `display: none`，**不是** `max-height: 0` + transition
- [ ] `:has()` 规则生效：折叠态 bar 无 `border-bottom`
- [ ] chevron 展开后 SVG `rotate(180deg)` 且 `aria-expanded` 同步
