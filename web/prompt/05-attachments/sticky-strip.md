# 便签条 · Sticky Strip

> compose bar 下方的便签展示区 · 胶带装饰 · 上标编号关联
> 视觉真值：`v-merged.html` · `.sticky-strip` / `.sticky` / `.sup`

---

## 1 · 设计理念

附件不是冰冷的文件列表，而是**贴在手稿底部的便签条**：
- 每张便签有顶部**胶带条**装饰（`::before` 伪元素）
- 入场时从缩小微旋到正位（`sticky-land`），像手贴上去的动作
- 三种类型用**胶带颜色**区分（灰/绿/赭），便签底色也有微妙差异
- 编号与正文内的上标圆圈一一对应——"脚注"关系

---

## 2 · Sticky Strip 结构

```html
<div class="sticky-strip" aria-label="已添加引用">
  <div class="sticky sticky--ws">
    <span class="sticky__num">1</span>
    <span class="sticky__name">agent-loop.hpp</span>
    <span class="sticky__type">ws</span>
    <button class="sticky__x" aria-label="移除引用 1">×</button>
  </div>
  <div class="sticky sticky--file">
    <span class="sticky__num">2</span>
    <span class="sticky__name">screenshot.png</span>
    <span class="sticky__type">128 kB</span>
    <button class="sticky__x" aria-label="移除引用 2">×</button>
  </div>
  <div class="sticky sticky--url">
    <span class="sticky__num">3</span>
    <span class="sticky__name">docs.anthropic.com/cache</span>
    <span class="sticky__type">url</span>
    <button class="sticky__x" aria-label="移除引用 3">×</button>
  </div>
</div>
```

**位置**：紧贴 `.compose__bar` 下方，共享左右边界。`border-top: none` + `margin-top: -1px` 融合上下边框。

---

## 3 · Sticky Strip CSS

```css
.sticky-strip {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
  padding: var(--space-3) var(--space-3) var(--space-2);
  background: var(--surface-sunken);
  border: 1px solid var(--rule-soft);
  border-top: none;
  border-radius: 0 0 var(--radius-md) var(--radius-md);
  margin-top: -1px;
  position: relative;
  z-index: 1;
}
```

**注意**：`z-index: 1` 低于 compose bar 的 `z-index: 2`，确保 bar 的底部边框在视觉上"盖住"strip 的顶部。

---

## 4 · 便签 CSS

### 4.1 Base

```css
.sticky {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 5px 8px 5px 10px;
  background: var(--surface);
  border-radius: var(--radius-sm);
  box-shadow: var(--shadow-1);
  position: relative;
  cursor: default;
  animation: sticky-land 240ms var(--ease) both;
}

.sticky:nth-child(2) { animation-delay: 50ms; }
.sticky:nth-child(3) { animation-delay: 100ms; }
.sticky:nth-child(4) { animation-delay: 150ms; }
```

### 4.2 胶带条

```css
.sticky::before {
  content: '';
  position: absolute;
  top: -3px;
  left: 50%;
  transform: translateX(-50%);
  width: 28px;
  height: 6px;
  border-radius: 1px;
  opacity: .45;
}
```

**关键**：胶带条突出便签顶部 3px（`top: -3px`），宽 28px，高 6px，圆角极小（1px），半透明（`.45`）。

### 4.3 类型变体

```css
/* 本地上传 — 灰胶带 · 白底 */
.sticky--file::before { background: var(--rule); }
.sticky--file { background: var(--surface); }

/* 工作区 — 绿胶带 · 淡绿底 */
.sticky--ws::before { background: var(--accent); }
.sticky--ws { background: oklch(from var(--accent-tint) l calc(c * .4) h); }

/* URL — 赭胶带 · 淡赭底 */
.sticky--url::before { background: var(--signal); }
.sticky--url { background: oklch(from var(--signal-tint) l calc(c * .4) h); }
```

**规则**：
- 胶带色 = 该类型的主色（rule / accent / signal），40% 不透明度
- 便签底色 = 该类型 tint 色的低饱和度版（`c * .4`），确保足够微妙

### 4.4 入场动画

```css
@keyframes sticky-land {
  from {
    opacity: 0;
    transform: scale(.88) rotate(-2deg);
  }
  to {
    opacity: 1;
    transform: scale(1) rotate(0);
  }
}
```

每张便签延迟 50ms 错开（`.sticky:nth-child(N) { animation-delay: N*50ms }`），最大延迟不超过 200ms。

### 4.5 内部元素

```css
.sticky__num {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 700;
  font-size: 11px;
  color: var(--signal);
  min-width: 14px;
  text-align: center;
  flex-shrink: 0;
}

.sticky__name {
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--content);
  max-width: 160px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.sticky__type {
  font-family: var(--font-mono);
  font-size: 9px;
  text-transform: uppercase;
  letter-spacing: .06em;
  color: var(--content-soft);
  flex-shrink: 0;
}
```

### 4.6 移除按钮

```css
.sticky__x {
  width: 16px;
  height: 16px;
  border: none;
  background: transparent;
  color: var(--content-soft);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 11px;
  border-radius: 50%;
  flex-shrink: 0;
  opacity: 0;
  transition: opacity var(--duration-fast) var(--ease),
              color var(--duration-fast) var(--ease),
              background var(--duration-fast) var(--ease);
}

.sticky:hover .sticky__x { opacity: 1; }
.sticky__x:hover {
  color: var(--signal);
  background: var(--signal-tint);
}
```

**交互**：× 按钮只在便签 hover 时显示（`opacity: 0 → 1`），hover 时变 signal 色 + signal-tint 底。

---

## 5 · 上标编号 · Superscript Markers

### 5.1 结构

```html
<span class="sup" data-ref="1" aria-label="引用 1: agent-loop.hpp">1</span>
```

上标出现在 compose 的富文本区域内，跟随在用户文字之间。

### 5.2 CSS

```css
.sup {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 17px;
  height: 17px;
  border-radius: 50%;
  background: var(--signal-tint);
  color: var(--signal-deep);
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 700;
  font-size: 10px;
  line-height: 1;
  vertical-align: super;
  margin: 0 1px;
  cursor: help;
  border: 1px solid oklch(from var(--signal) l c h / .15);
  transition: background var(--duration-fast) var(--ease);
}

.sup:hover {
  background: oklch(from var(--signal-tint) calc(l - .04) c h);
}
```

### 5.3 关联逻辑

上标编号与 sticky-strip 中的便签通过 `data-ref` 一一对应：

```js
// Hover 上标 → 高亮对应便签
document.querySelectorAll('.sup').forEach(sup => {
  sup.addEventListener('mouseenter', () => {
    const idx = sup.getAttribute('data-ref');
    highlightSticky(idx);
  });
  sup.addEventListener('mouseleave', () => {
    clearStickyHighlight();
  });
});

function highlightSticky(idx) {
  const sticky = document.querySelector(`.sticky[data-ref="${idx}"]`);
  if (sticky) sticky.classList.add('is-highlighted');
}
```

```css
.sticky.is-highlighted {
  box-shadow: 0 0 0 2px oklch(from var(--signal) l c h / .25), var(--shadow-1);
}
```

### 5.4 上标的可选性

上标编号是**可选的**。两种使用模式：

| 模式 | 场景 | 行为 |
|---|---|---|
| 隐式附件 | 用户只想"带上文件" | 添加便签，不插入上标。正文无编号 |
| 显式引用 | 用户想在文中指向特定文件 | 添加便签 + 在光标位置插入上标 |

默认行为：从 citation lookup 选中 → 隐式附件（只加便签）。
如果用户在 textarea 中输入 `@` 触发 → 显式引用（便签 + 上标）。

---

## 6 · 移除引用

```js
function removeAttachment(index) {
  // 1. 移除便签 DOM
  const sticky = document.querySelector(`.sticky[data-ref="${index}"]`);
  sticky?.remove();

  // 2. 移除对应上标
  const sup = document.querySelector(`.sup[data-ref="${index}"]`);
  sup?.remove();

  // 3. 重新编号剩余便签和上标
  renumberAttachments();

  // 4. 如果没有附件了，移除 sticky-strip
  if (attachments.length === 0) {
    document.querySelector('.sticky-strip')?.remove();
  }
}
```

**重新编号**：移除中间的引用后，后续编号前移（`[1] [3]` → `[1] [2]`），保持连续。

---

## 7 · 数据模型

```ts
interface Attachment {
  index: number;          // 1-based，显示用
  type: 'file' | 'workspace' | 'url';
  name: string;           // 显示名（文件名 / 路径尾 / 域名+path）
  displaySize: string;    // "4.2 kB" / "URL" / "128 kB"
  // type-specific
  file?: File;            // type=file
  path?: string;          // type=workspace
  url?: string;           // type=url
}
```

发送消息时，附件随消息一起提交：

```js
async function sendMessageWithAttachments(message, attachments) {
  const formData = new FormData();
  formData.append('message', message);
  formData.append('agent_key', currentAgentKey);
  formData.append('session_id', currentSessionId);

  for (const att of attachments) {
    if (att.type === 'file') {
      formData.append('files', att.file, att.name);
    } else if (att.type === 'workspace') {
      formData.append('workspace_files', att.path);
    } else if (att.type === 'url') {
      formData.append('urls', att.url);
    }
  }

  const res = await fetch('/api/v1/chat', {
    method: 'POST',
    body: formData,
  });
  handleSSEStream(res.body);
}
```

---

## 8 · Strip 的显隐

- **无附件时**：`.sticky-strip` 不存在（不渲染空容器）
- **第一个附件添加时**：创建 `.sticky-strip` + 插入第一张便签
- **最后一个附件移除时**：`.sticky-strip` 移除
- **发送消息后**：所有附件和 strip 清除

---

## 9 · 禁止事项

- ❌ 便签用圆角 pill 形状 —— 用 `radius-sm`（4px），不是 999px
- ❌ 胶带条用实色（opacity: 1）—— 必须半透明（`.45`）
- ❌ 便签底色饱和度太高 —— `c * .4` 是上限
- ❌ 上标编号超过单位数时用两位数字（如 `12`）—— 如果超过 9 个附件，编号显示为 `9+` 并不再自动插入上标
- ❌ 移除按钮常驻显示 —— 只在 hover 时出现
- ❌ 便签入场用 slide-in —— 只用 scale + rotate 的 `sticky-land`
- ❌ 便签高度超过 36px —— 保持单行紧凑
