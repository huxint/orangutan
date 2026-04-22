# App Shell · Grid · 状态机 · 分隔条

> 从这里读起。定义**整个骨架的骨骼** —— 外层 HTML 结构、CSS Grid、命名区域、打开 / 关闭状态、分隔条行为、响应式策略。
> 所有后续文档（top-nav / left-panel / middle-chat / right-panel）都假设本结构已就位。

---

## 1 · HTML 根结构

唯一允许的外层骨架：

```html
<html lang="zh-CN" data-theme="light" data-right="closed">
  <body>
    <div class="app">
      <header class="top-nav">...</header>
      <main class="workspace">
        <aside class="left-panel" id="left-panel">...</aside>
        <div class="divider divider--left" role="separator"
             aria-orientation="vertical" aria-controls="left-panel"
             aria-valuenow="260" aria-valuemin="220" aria-valuemax="360"
             tabindex="0"></div>
        <section class="chat-region">...</section>
        <div class="divider divider--right" role="separator"
             aria-orientation="vertical" aria-controls="right-panel"
             aria-valuenow="320" aria-valuemin="280" aria-valuemax="480"
             tabindex="0"></div>
        <aside class="right-panel" id="right-panel" aria-hidden="true">...</aside>
      </main>
    </div>
  </body>
</html>
```

**必守**：
- `<html>` 携带 `data-theme` 和 `data-right` — 全局状态机锚点
- 分隔条是 `<div role="separator">`，支持 `tabindex="0"` 用键盘 ←→ 调整宽度
- `<aside>` 用于左右语义容器，`<section>` 用于中栏
- 顺序**不能变**（left → divider → chat → divider → right）—— CSS Grid 依赖此顺序

---

## 2 · CSS Grid 定义

### 根盒

```css
html, body {
  margin: 0;
  height: 100%;
  overflow: hidden;        /* 禁止 body 滚动 · 所有滚动在内部容器 */
  background: var(--surface);
  color: var(--content);
}

.app {
  display: flex;
  flex-direction: column;
  height: 100vh;
}

.top-nav {
  height: var(--nav-h, 52px);
  flex-shrink: 0;
}

.workspace {
  flex: 1;
  display: grid;
  grid-template-columns:
    var(--left-w, 260px)
    1px
    1fr
    1px
    var(--right-w-closed, 0px);
  overflow: hidden;
  transition: grid-template-columns var(--duration-slow, 320ms) var(--ease);
}

[data-right="open"] .workspace {
  grid-template-columns:
    var(--left-w, 260px)
    1px
    1fr
    1px
    var(--right-w-open, 320px);
}
```

### 布局专属 tokens

追加到全局 tokens（不覆盖 `web/prompt/01-visual-language/design-tokens.md`，仅新增）：

```css
:root {
  --nav-h: 52px;

  --left-w-min: 220px;
  --left-w-max: 360px;
  --left-w:     260px;            /* 可持久化 · localStorage */

  --right-w-min: 280px;
  --right-w-max: 480px;
  --right-w-open:   320px;        /* 可持久化 */
  --right-w-closed: 0px;
}
```

**为什么 left-w 范围是 220-360？** 卡片式 Contributors 需要至少 220px 才放得下 "思考中 · 3 tools · 4s" 这种 meta 文字；360px 之上 cards 会显得稀松。超出此区间的拖拽被 clamp。

### 状态机

两个顶层状态锚：
- `html[data-theme="light|dark"]` — 主题
- `html[data-right="open|closed"]` — 右栏是否展开

**无中间态需要显式管理** —— CSS `transition` 自动处理过渡。

持久化到 `localStorage`：

| key | 值 | 何时写入 |
|---|---|---|
| `orangutan.theme` | `light` / `dark` | theme toggle 点击 |
| `orangutan.layout.right` | `open` / `closed` | 右栏切换 |
| `orangutan.layout.right.teammate` | teammate key | 打开右栏时 |
| `orangutan.layout.--left-w` | px 数字 | 分隔条拖拽结束 |
| `orangutan.layout.--right-w-open` | px 数字 | 分隔条拖拽结束 |

启动时从 localStorage 读取；若缺失则使用 CSS 默认。

---

## 3 · 区域子规则

| 区域 | 行为 | 滚动 |
|---|---|---|
| `.top-nav` | 固定 52px；内部 flex 两端对齐 | 无 |
| `.left-panel` | `overflow-y: auto`；宽度由 `--left-w` 决定 | **独立滚动** |
| `.chat-region` | flex column；内部 `.chat-scroll` + `.chat-footer` | 仅 `.chat-scroll` 滚动 |
| `.right-panel` | `overflow-y: auto`；宽度由 grid 控制；`transition: opacity` | 独立滚动 |

---

## 4 · 分隔条（Divider）

### CSS

```css
.divider {
  background: var(--rule-soft);
  position: relative;
  transition: background var(--duration-fast, 120ms) var(--ease);
}

/* 扩大点击 / 拖拽热区到 8px */
.divider::before {
  content: '';
  position: absolute;
  top: 0; bottom: 0;
  left: -4px; right: -4px;
  cursor: col-resize;
}

/* 中央抓握点 · 仅 hover / focus 显现 */
.divider::after {
  content: '';
  position: absolute;
  top: 50%;
  left: 50%;
  width: 3px;
  height: 32px;
  border-radius: 2px;
  background: var(--rule);
  transform: translate(-50%, -50%) scaleX(0);
  transition: transform var(--duration-med, 200ms) var(--ease);
}

.divider:hover,
.divider:focus-visible {
  background: var(--rule);
  outline: none;
}
.divider:hover::after,
.divider:focus-visible::after {
  transform: translate(-50%, -50%) scaleX(1);
}

.divider.is-dragging {
  background: var(--content-muted);
}
.divider.is-dragging::after {
  transform: translate(-50%, -50%) scaleX(1);
}

/* 右栏关闭时禁用右侧分隔条 */
[data-right="closed"] .divider--right {
  pointer-events: none;
}
```

### 拖拽逻辑（JS）

```js
function enableDivider(divEl, targetVar, min, max) {
  let startX = 0;
  let startW = 0;

  divEl.addEventListener('pointerdown', (e) => {
    divEl.setPointerCapture(e.pointerId);
    startX = e.clientX;
    startW = parseInt(
      getComputedStyle(document.documentElement).getPropertyValue(targetVar)
    );
    divEl.classList.add('is-dragging');
    document.body.style.userSelect = 'none';
    document.body.style.cursor = 'col-resize';
    // 临时禁用 workspace 过渡 · 否则抖动
    document.querySelector('.workspace').style.transition = 'none';
  });

  divEl.addEventListener('pointermove', (e) => {
    if (!divEl.hasPointerCapture(e.pointerId)) return;
    const delta = targetVar === '--left-w'
      ? (e.clientX - startX)
      : (startX - e.clientX);
    const next = Math.min(max, Math.max(min, startW + delta));
    document.documentElement.style.setProperty(targetVar, next + 'px');
    divEl.setAttribute('aria-valuenow', String(next));
  });

  divEl.addEventListener('pointerup', (e) => {
    divEl.releasePointerCapture(e.pointerId);
    divEl.classList.remove('is-dragging');
    document.body.style.userSelect = '';
    document.body.style.cursor = '';

    const finalW = parseInt(
      getComputedStyle(document.documentElement).getPropertyValue(targetVar)
    );
    localStorage.setItem(`orangutan.layout.${targetVar}`, String(finalW));

    // 恢复过渡（下一帧避免抖动）
    requestAnimationFrame(() => {
      document.querySelector('.workspace').style.transition = '';
    });
  });
}

enableDivider(document.querySelector('.divider--left'),
  '--left-w', 220, 360);
enableDivider(document.querySelector('.divider--right'),
  '--right-w-open', 280, 480);
```

### 双击重置

```js
divEl.addEventListener('dblclick', () => {
  document.documentElement.style.removeProperty(targetVar);
  localStorage.removeItem(`orangutan.layout.${targetVar}`);
  // 触发一次过渡让用户感知重置
  document.querySelector('.workspace').style.transition = '';
});
```

---

## 5 · 响应式断点

V1 只在窄屏触发**一次**结构重排（本步骤不实现 mobile，只定义触发规则）：

| 视口宽度 | 行为 |
|---|---|
| ≥ 1280px | 全尺寸三栏 · 左 260 / 右 320 · 默认 |
| 960–1279px | 三栏保留 · 左可压到 220 · 右栏展开时中栏 min-width 由 560 放宽到 520 |
| 768–959px | 左栏自动折叠为 rail 72px（hover 展开浮层，Step I 细化） · 右栏展开改 overlay |
| < 768px | 触发"移动端重排"—— 本 V1 不处理，显示"使用桌面版获得完整体验"提示 |

CSS 媒介查询占位（留空但定义好选择器）：
```css
@media (max-width: 1279px) {
  :root { --left-w-min: 220px; }
}
@media (max-width: 959px) {
  /* Step I 填充 rail / overlay */
}
```

---

## 6 · 右栏打开 / 关闭

```js
function openRightPanel(teammateKey) {
  document.documentElement.setAttribute('data-right', 'open');
  document.getElementById('right-panel').setAttribute('aria-hidden', 'false');
  renderTeammateDetail(teammateKey);   // 见 right-panel.md
  localStorage.setItem('orangutan.layout.right', 'open');
  localStorage.setItem('orangutan.layout.right.teammate', teammateKey);

  // 焦点延迟移入右栏 · 等 grid 过渡结束
  setTimeout(() => {
    document.querySelector('.right-panel').focus();
  }, 280);
}

function closeRightPanel() {
  document.documentElement.setAttribute('data-right', 'closed');
  document.getElementById('right-panel').setAttribute('aria-hidden', 'true');

  // 焦点回归激活的 team-card
  document.querySelector('.team-card.active')?.focus();
  localStorage.setItem('orangutan.layout.right', 'closed');
}
```

**必守**：关闭右栏时**不清除** `right.teammate`——下次打开仍展示同一 teammate。用户必须显式切换才变。

### 过渡时序

- `.workspace` grid-template-columns 过渡 **320ms · cubic-bezier(0.2, 0.7, 0.2, 1)**
- `.right-panel` opacity 过渡 **240ms · 同缓动** — 内容比结构扩展稍晚淡入

```css
.right-panel {
  opacity: 0;
  transition: opacity 240ms var(--ease);
}
[data-right="open"] .right-panel { opacity: 1; }
```

---

## 7 · 滚动容器规则

- `body` 永远 `overflow: hidden` — 没有页面级滚动
- `.chat-scroll` 是主要滚动容器
- `.left-panel` 和 `.right-panel` 各自 `overflow-y: auto`
- **禁用** `scroll-behavior: smooth` 作为默认（仅在键盘跳转 / 回到底部等显式操作启用）

**绝不使用 `scrollIntoView()`** — 见 `interactions.md · §7`。

---

## 8 · 颜色 / 字体 / 空间

不在本文件重复——引用 [`web/prompt/01-visual-language/design-tokens.md`](../01-visual-language/design-tokens.md)。
本步骤**只新增** §2 末尾列出的布局专属 tokens。

---

## 9 · 完工自检

- [ ] `<html>` 有 `data-theme` 与 `data-right` 属性
- [ ] `.workspace` 使用 CSS Grid，5 列（左 · div · 中 · div · 右）
- [ ] `body { overflow: hidden }` 且只有 `.chat-scroll` / `.left-panel` / `.right-panel` 滚动
- [ ] 分隔条可拖拽，clamp 到 220-360 / 280-480 范围
- [ ] 双击重置分隔宽度
- [ ] 拖拽时整体不抖动（transition 被临时禁用）
- [ ] 打开 / 关闭右栏 + 分隔宽度 + 主题 都持久化到 localStorage
- [ ] 主题切换不打破布局
- [ ] `aria-hidden` 在右栏关闭时为 `true`
- [ ] 关闭右栏时焦点回到激活的 team-card
