# 顶栏 · Top Nav

> 52px 高 · 单行 · 两端对齐 · 承载身份 / 会话 / 配置入口 / 主题 / 用户
> 视觉真值：`v1-column.html` · `.top-nav`

---

## 1 · 尺寸与约束

| 属性 | 值 |
|---|---|
| 高度 | `--nav-h: 52px`（固定） |
| 背景 | `var(--surface)` |
| 底边 | `border-bottom: 1px solid var(--rule-soft)` |
| 水平内边距 | `0 var(--space-5)` = 24px |
| z-index | `var(--z-sticky, 10)`（被 top-nav popover 压过） |

---

## 2 · 结构

```html
<header class="top-nav">
  <div class="top-nav__left">
    <!-- logo + wordmark + session-chip -->
  </div>
  <div class="top-nav__right">
    <!-- token meter · action chips · divider · icon btns · user avatar -->
  </div>
</header>
```

```css
.top-nav {
  height: var(--nav-h);
  flex-shrink: 0;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 var(--space-5);
  background: var(--surface);
  border-bottom: 1px solid var(--rule-soft);
}
.top-nav__left  { display: flex; align-items: center; gap: var(--space-4); }
.top-nav__right { display: flex; align-items: center; gap: var(--space-2); }
```

---

## 3 · 左侧三件套

### 3.1 Logo（占位符）

```html
<div class="logo" aria-hidden="true"><em>O</em></div>
```
```css
.logo {
  width: 28px; height: 28px;
  border-radius: 50%;
  background: var(--signal-tint);
  border: 1px solid oklch(55% 0.13 42 / .35);
  display: flex;
  align-items: center; justify-content: center;
  font-family: var(--font-display);
  font-style: italic; font-weight: 600;
  color: var(--signal-deep);
  font-size: 14px;
}
```

真实 Orangutan 资产待设计，**禁止临时 AI 生成**—— 使用占位符直到设计交付。

### 3.2 Wordmark

```html
<div class="wordmark"><em>Orangutan</em></div>
```
```css
.wordmark {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 20px;
  letter-spacing: -.01em;
}
```

### 3.3 Session chip

```html
<button class="session-chip" type="button" aria-haspopup="menu">
  <span class="dot" aria-hidden="true"></span>
  <span class="session-chip__label">session · main-thread · 22:14</span>
</button>
```
```css
.session-chip {
  font-family: var(--font-mono);
  font-size: var(--text-caption);
  background: var(--surface-raised);
  color: var(--content-muted);
  padding: 4px 10px;
  border-radius: 999px;
  border: none;
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: var(--space-2);
  max-width: 320px;
}
.session-chip:hover { color: var(--content); }
.session-chip .dot {
  width: 5px; height: 5px;
  border-radius: 50%;
  background: var(--accent);     /* 活跃 session */
  flex-shrink: 0;
}
.session-chip[data-state="idle"] .dot { background: var(--content-soft); }
.session-chip__label {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
```

**数据绑定**：
- 标题：`session.title || session.id.slice(0, 12)`（`/api/v1/sessions/:id`）
- 时间：相对时间（"22:14" / "2h ago" / "yesterday"）
- dot 颜色：当前 session 是否活跃（chat SSE 在流）

点击打开 session 切换弹层（Step I 实现）；本步骤仅注册 `aria-haspopup="menu"` 与空处理。

---

## 4 · 右侧操作栏

### 4.1 Token meter（永远在最左）

```html
<button class="nav-chip primary" type="button" data-usage="normal">
  <span class="num">2,431</span> tokens · 32%
</button>
```
```css
.nav-chip {
  font-family: var(--font-mono);
  font-size: 11px;
  font-weight: 500;
  background: transparent;
  color: var(--content-muted);
  padding: 6px 10px;
  border: 1px solid transparent;
  border-radius: var(--radius-sm);
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 6px;
  transition:
    background var(--duration-fast) var(--ease),
    color var(--duration-fast) var(--ease),
    border-color var(--duration-fast) var(--ease);
}
.nav-chip:hover { background: var(--surface-raised); color: var(--content); }
.nav-chip.primary { color: var(--content); font-weight: 500; }
.nav-chip.primary .num { color: var(--accent-deep); }
.nav-chip.primary[data-usage="high"] .num     { color: var(--signal-deep); }
.nav-chip.primary[data-usage="critical"] .num { color: var(--signal); }
```

`data-usage` 阈值：
- `normal`：≤ 60% context 使用
- `high`：60-85%
- `critical`：> 85%

**数据绑定**：
- `num`：累计 tokens（格式 `1,234` / `12.4k` / `1.2M`）
- `32%`：当前 session 占 context_window 的百分比
- 来源：`/api/v1/system` + 每轮 chat SSE 累计

### 4.2 入口 chips

顺序固定（不允许换）：
```html
<button class="nav-chip">Skills</button>
<button class="nav-chip">MCP</button>
<button class="nav-chip">Memory</button>
<button class="nav-chip">Providers</button>
<button class="nav-chip">Workspace</button>
```

每个 chip 点击打开独立面板（Step G）。本步骤只提供按钮壳 + `onclick` 留 TODO。

**带计数的变体**（可选，若后端提供）：
```html
<button class="nav-chip">Skills <span class="count">12</span></button>
```
```css
.nav-chip .count {
  color: var(--content-soft);
  font-size: 10px;
  margin-left: 2px;
}
```

计数来源：
- Skills → `/api/v1/skills` 中 `active=true` 的数量
- MCP → 后端 TBD
- Memory → 后端 TBD
- Providers → `/api/v1/config` 中 `profiles` 的键数
- Workspace → 后端 TBD

若无计数 API 就只显示 label，**不显示 `0`**。

### 4.3 竖分隔

```html
<div class="nav-divider" role="separator" aria-orientation="vertical"></div>
```
```css
.nav-divider {
  width: 1px; height: 16px;
  background: var(--rule-soft);
  margin: 0 var(--space-2);
}
```

### 4.4 图标按钮

```html
<button class="icon-btn" aria-label="打开设置">⚙</button>
<button class="icon-btn" aria-label="切换明暗主题" data-theme-toggle>◐</button>
<div class="user-avatar" role="button" tabindex="0" aria-label="账户菜单"><em>H</em></div>
```
```css
.icon-btn {
  width: 32px; height: 32px;
  border-radius: var(--radius-sm);
  background: transparent;
  border: none;
  color: var(--content-muted);
  cursor: pointer;
  display: flex;
  align-items: center; justify-content: center;
  font-family: var(--font-mono);
  font-size: 13px;
  transition:
    background var(--duration-fast) var(--ease),
    color var(--duration-fast) var(--ease);
}
.icon-btn:hover { background: var(--surface-raised); color: var(--content); }
.icon-btn:focus-visible {
  outline: none;
  box-shadow: 0 0 0 2px var(--surface), 0 0 0 4px oklch(from var(--signal) l c h / .3);
}

.user-avatar {
  width: 32px; height: 32px;
  border-radius: 50%;
  background: var(--surface-sunken);
  display: flex;
  align-items: center; justify-content: center;
  font-family: var(--font-display);
  font-style: italic; font-weight: 600;
  color: var(--content);
  font-size: 14px;
  cursor: pointer;
}
.user-avatar:focus-visible {
  outline: none;
  box-shadow: 0 0 0 2px var(--surface), 0 0 0 4px oklch(from var(--signal) l c h / .3);
}
```

**图标占位符**：`⚙` / `◐` 是 Unicode 占位。实现时用 **Phosphor Icons** 的 Gear / CircleHalf line 变体替换，保持 18×18 尺寸。

---

## 5 · 交互行为

### 5.1 主题切换

本步骤仅瞬时切换（Step H 将覆盖为圆圈掠过动画）：
```js
document.querySelector('[data-theme-toggle]').addEventListener('click', () => {
  const cur = document.documentElement.getAttribute('data-theme');
  const next = cur === 'light' ? 'dark' : 'light';
  document.documentElement.setAttribute('data-theme', next);
  localStorage.setItem('orangutan.theme', next);
});
```

初始化时读取：
```js
const savedTheme = localStorage.getItem('orangutan.theme') || 'light';
document.documentElement.setAttribute('data-theme', savedTheme);
```

### 5.2 Session chip 菜单（Step I）

点击打开 popover 含最近 5 个 sessions + "+ New" + "⌘K Search"。本步骤占位。

### 5.3 User avatar 菜单（Step I）

点击 / Enter 打开账户菜单：配置 · 登出 · 关于。本步骤占位。

### 5.4 溢出处理

当视口 < 1100px，从右往左依次折叠成 overflow menu（"..."）：

折叠顺序：Providers → Workspace → MCP → Memory → Skills

保留可见：Token meter · Settings · Theme · Avatar

本步骤不实现溢出菜单 —— `.top-nav__right` 的 `flex-wrap: nowrap`，允许水平溢出 scroll-x；Step I 细化。

---

## 6 · API 数据绑定速查

| 展示 | 来源 |
|---|---|
| Session 名 / 时间 / 活跃 | `/api/v1/sessions/:id` + chat SSE |
| Token 用量 / 百分比 | `/api/v1/system` + 逐轮 chat SSE |
| Skills count | `/api/v1/skills?active=1` |
| MCP count | 后端 TBD |
| Memory count | 后端 TBD |
| Providers count | `/api/v1/config.profiles` 键数 |
| Agent / user 身份 | 本地 config 或 `/api/v1/me` · 后端 TBD |

---

## 7 · 可达性

- `.logo`：装饰 · `aria-hidden="true"`
- `.wordmark`：装饰文本 · 对屏幕阅读器无害
- 图标按钮：**必须**有 `aria-label`
- Theme toggle：`aria-pressed` 在 dark 下为 `true`
- Session chip：`aria-haspopup="menu"`

---

## 8 · 禁止事项

- ❌ 任何 chip 里出现 emoji（如 🔥 💡 🎉）
- ❌ 把 logo 换成真实 Orangutan illustration（占位符 + 未来替换）
- ❌ 彩色渐变做 logo 背景
- ❌ 顶栏高度 > 64 或 < 48
- ❌ 在顶栏塞搜索框或其他大控件
- ❌ Session chip 超 320px 宽（超长标题 ellipsis）

---

## 9 · 自检

- [ ] 高度恰好 52px
- [ ] 左右两端对齐
- [ ] Token meter 的 `num` 根据 `data-usage` 染色（normal accent / high/critical signal）
- [ ] 所有图标按钮有 `aria-label`
- [ ] Theme toggle 点击后 `data-theme` 切换并持久化
- [ ] 视口 < 1100 时 user-avatar 不被顶飞（`flex-shrink: 0`）
- [ ] dark 主题下 `border-bottom` 仍清晰可见
