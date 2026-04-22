# 交互 · 键盘 · 过渡 · 无障碍 · 自检

> 跨组件共通的交互契约。所有 Step B 的实现者必须通读本文件。

---

## 1 · 键盘快捷键

| 键 | 作用 | 触发位置 |
|---|---|---|
| **Enter** | 发送消息 | 焦点在 `.send-bar__input` |
| **Shift + Enter** | 换行 | 焦点在 `.send-bar__input` |
| **Esc** | 中断 in-flight chat / 关闭右栏 / 关闭 popover | 全局（按优先级处理，见 §2） |
| **⌘ K** / **Ctrl K** | 打开搜索 / session 切换（Step I） | 全局 |
| **⌘ /** / **Ctrl /** | 打开快捷键帮助浮窗（Step I） | 全局 |
| **⌘ ,** / **Ctrl ,** | 打开设置（Step G） | 全局 |
| **⌘ B** / **Ctrl B** | 切换右栏 open/closed | 全局 |
| **⌘ Shift L** | 切换主题 | 全局 |
| **Tab** | 焦点前进（button / input / link / team-card / divider） | 全局 |
| **Shift + Tab** | 焦点后退 | 全局 |
| **Space / Enter** | 激活焦点所在按钮 | 任意 button |
| **← / →** | 调整分隔条宽度（每步 8px） | 焦点在 `.divider` |
| **↑ / ↓** | 在 team-card 列表内移动焦点 | 焦点在 `.team-card` |
| **Home / End** | 跳到聊天最底 / 最上 | 焦点在 `.chat-scroll` |

JS 骨架：

```js
document.addEventListener('keydown', (e) => {
  const target = e.target;
  const isTyping = target.tagName === 'INPUT'
    || target.tagName === 'TEXTAREA'
    || target.isContentEditable;

  // Esc 处理优先级
  if (e.key === 'Escape') {
    if (popoverOpen()) { closePopover(); return; }
    if (document.documentElement.getAttribute('data-right') === 'open') {
      closeRightPanel();
      return;
    }
    const chatState = document.querySelector('.chat-region').getAttribute('data-chat-state');
    if (chatState === 'in-flight') { abortChat(); return; }
  }

  // 全局快捷键（含 ⌘/Ctrl 才触发，避免打字命中）
  if (e.metaKey || e.ctrlKey) {
    switch (e.key.toLowerCase()) {
      case 'k': e.preventDefault(); openSessionSearch(); break;
      case '/': e.preventDefault(); openShortcutHelp(); break;
      case ',': e.preventDefault(); openSettings(); break;
      case 'b': e.preventDefault(); toggleRightPanel(); break;
      case 'l': if (e.shiftKey) { e.preventDefault(); toggleTheme(); } break;
    }
  }
});
```

---

## 2 · Esc 键优先级

按从高到低处理：
1. 关闭当前打开的 popover / dialog
2. 关闭右栏（若 open）
3. 中断 in-flight chat（若 chat 状态是 `in-flight`）
4. 否则无动作

---

## 3 · 过渡时序

所有过渡统一使用 CSS 变量：

```css
:root {
  --duration-fast: 120ms;   /* hover · focus · color 切换 */
  --duration-med:  200ms;   /* opacity · transform 快速 */
  --duration-slow: 320ms;   /* grid-template-columns · 主结构 */
  --ease:          cubic-bezier(0.2, 0.7, 0.2, 1);
}
```

| 对象 | 时长 | 缓动 |
|---|---|---|
| 按钮 hover 背景 | 120ms | ease |
| 按钮 focus ring | 120ms | ease |
| 右栏宽度（grid） | 320ms | ease |
| 右栏内容 opacity | 240ms | ease |
| 分隔条 hover 背景 | 120ms | ease |
| 分隔条抓握点显现 | 200ms | ease |
| back-to-bottom 浮按钮 | 200ms | ease |
| team-card 进入（slide-in） | 280ms | ease |
| 主题切换（本步骤瞬时，Step H 覆盖为圆圈） | 0ms | — |

---

## 4 · `prefers-reduced-motion` 兜底

**必须**实现：

```css
@media (prefers-reduced-motion: reduce) {
  *, *::before, *::after {
    animation-duration: 0.01ms !important;
    animation-iteration-count: 1 !important;
    transition-duration: 0.01ms !important;
    scroll-behavior: auto !important;
  }
}
```

特别注意 `@keyframes pulse` / `flash` 应被覆盖（上面 `*` 通配即可）。

---

## 5 · 焦点管理

### 5.1 可 focus 元素清单

| 元素 | tabindex | 焦点环 |
|---|---|---|
| `.session-chip` | 0（天然） | 标准 |
| `.nav-chip` | 0 | 标准 |
| `.icon-btn` | 0 | 标准 |
| `.user-avatar` | 0（显式） | 标准 |
| `.team-card` | 0（天然 · button） | 强调 |
| `.add-card` | 0 | 标准 |
| `.divider` | 0（显式） | 标准 |
| `.send-bar__input` | 0 | 无环（输入本身视觉足够） |
| `.send-bar__send` / `.send-bar__attach` | 0 | 标准 |
| `.detail-close` | 0 | 标准 |
| `.back-to-bottom` | 0 | 标准 |
| `.right-panel`（关闭时） | -1 | — |

### 5.2 焦点环统一样式

```css
*:focus-visible {
  outline: none;
  box-shadow:
    0 0 0 2px var(--surface),
    0 0 0 4px oklch(from var(--signal) l c h / .3);
}

/* 禁用元素无焦点环 */
*[disabled]:focus-visible { box-shadow: none; }
```

部分组件可覆盖（如 `.divider`、`.chat-footer__inner .send-bar__input`）。

### 5.3 打开 / 关闭右栏的焦点流

- 打开：焦点移入右栏（延迟 280ms 等过渡）
  ```js
  setTimeout(() => document.querySelector('.right-panel').focus(), 280);
  ```
- 关闭：焦点回到激活的 team-card
  ```js
  document.querySelector('.team-card[aria-current="true"]')?.focus();
  ```

---

## 6 · 无障碍

### 6.1 ARIA 清单

| 组件 | 属性 |
|---|---|
| `.top-nav` | 无额外（header 即可） |
| `.session-chip` | `aria-haspopup="menu"` |
| `.icon-btn` | `aria-label` 必填 · `aria-pressed` 用于 toggle |
| `.theme-toggle` | `aria-pressed` 为 `true` 当 `data-theme="dark"` |
| `.team-card` | `aria-current` 为 `"true"` / `"false"` |
| `.divider` | `role="separator"` · `aria-orientation` · `aria-controls` · `aria-valuenow/min/max` |
| `.right-panel` | `aria-hidden` 切 · `tabindex="-1"` |
| `.status-row` | `aria-live="polite"` |
| `.send-bar__input` | `aria-label="消息输入"` |
| `.detail-close` | `aria-label="关闭详情"` |

### 6.2 色对比度

参见 [`01-visual-language/guardrails.md · §2`](../01-visual-language/guardrails.md)：
- 正文 on surface：≥ 7:1
- 辅助 on surface：≥ 4.5:1
- 图标按钮在 focus 时 ring 对比度 ≥ 3:1

### 6.3 尺寸

所有可点击目标：
- 桌面最小 **32px × 32px**（ring 扩 4px 视觉到 40px）
- 移动最小 **44px × 44px**（Step I）

---

## 7 · 滚动规则（重申）

1. `body { overflow: hidden }` · 没有页面滚动
2. 主滚动容器：`.chat-scroll`
3. 副滚动容器：`.left-panel` / `.right-panel`
4. `.chat-scroll` 的 sticky-to-bottom 逻辑（`middle-chat.md · §2`）
5. **绝对不使用 `scrollIntoView()` · 任何地方**
6. 若要跳到某个元素，使用：
   ```js
   const rect = el.getBoundingClientRect();
   const parentRect = scrollContainer.getBoundingClientRect();
   scrollContainer.scrollTop += rect.top - parentRect.top;
   ```

---

## 8 · 响应式降级（断点处理规则）

本 Step B 只实现 ≥ 1280px 的桌面默认。断点的行为契约：

```css
@media (max-width: 1279px) {
  :root { --left-w-min: 220px; }       /* 不动 default */
}
@media (max-width: 959px) {
  /* Step I 实现 left rail + right overlay 自动切换 */
}
@media (max-width: 767px) {
  /* Step I 实现移动端重排 · 本步骤显示静态提示 */
  .mobile-block {
    display: flex !important;
    position: fixed; inset: 0;
    background: var(--surface);
    z-index: 999;
    align-items: center; justify-content: center;
    padding: var(--space-6);
    font-family: var(--font-body);
    font-style: italic;
    color: var(--content-muted);
    text-align: center;
  }
}
.mobile-block { display: none; }
```

```html
<div class="mobile-block">
  此应用当前需要桌面浏览器（宽度 ≥ 768px）以获得完整体验。
</div>
```

---

## 9 · 完工自检（跨组件）

### 结构
- [ ] `<html>` 有 `data-theme` + `data-right`
- [ ] body `overflow: hidden`
- [ ] workspace 使用 CSS Grid 五列
- [ ] 分隔条可键盘 ←→ 调整（每步 8px）
- [ ] 双击分隔条重置宽度
- [ ] `--left-w` clamp 到 220-360
- [ ] `--right-w-open` clamp 到 280-480

### 状态持久化
- [ ] 主题持久化（localStorage `orangutan.theme`）
- [ ] 右栏开 / 关持久化
- [ ] 右栏选中 teammate 持久化
- [ ] 左 / 右栏拖拽宽度持久化

### 键盘
- [ ] Enter 发送 / Shift+Enter 换行 · IME 不误触
- [ ] Esc 按 §2 优先级处理
- [ ] ⌘B / Ctrl+B 切右栏
- [ ] ⌘⇧L 切主题
- [ ] Tab 顺序符合视觉顺序

### 过渡
- [ ] 所有 transition 使用 token
- [ ] `prefers-reduced-motion` 禁用所有动效
- [ ] 拖拽分隔时 `.workspace transition: none`
- [ ] 主题切换无布局重排

### 无障碍
- [ ] 所有图标按钮有 `aria-label`
- [ ] 右栏 `aria-hidden` 正确同步
- [ ] 焦点环 `:focus-visible` 所有组件存在且可见
- [ ] `.status-row` `aria-live="polite"`
- [ ] 对比度通过 AA（正文 AAA）

### 滚动
- [ ] grep 全项目无 `scrollIntoView`
- [ ] 主滚动贴底 · 上滚不回拉
- [ ] 左右栏独立滚动 · 不被 body 牵连

### 持久化数据恢复
- [ ] 刷新页面后恢复：主题 · 右栏开关 · 选中的 teammate · 分隔宽度

---

## 10 · 已知技术债

记录本步骤不解决、但下游步骤必须面对的约束：

| 债项 | 下游步骤 | 说明 |
|---|---|---|
| teammate 独立 event 流 | Step E（与后端协作） | 当前 teammate 活动需从主 agent 的 tool_end content 里解析 —— 后端应让 orchestration runtime 为 worker 发独立 chat.* 事件 |
| 溢出菜单 | Step I | 顶栏 < 1100px 的 chips 折叠目前是 scroll-x |
| 移动端布局 | Step I | 本步骤仅占位 `.mobile-block` |
| 主题切换动画 | Step H | 本步骤仅瞬时切换 |
| Session popover 菜单 | Step I | session-chip 点击目前仅注册事件，无弹层 |
| 附件面板 | Step F | attach 按钮仅占位 |
