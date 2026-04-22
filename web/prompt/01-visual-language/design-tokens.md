# 设计令牌 · Design Tokens

> 所有视觉量化参数。**组件样式只能引用这些令牌，不能硬编码色值 / 字号 / 间距。**

---

## 1 · CSS 变量声明模板

把以下代码完整抄入全局样式入口（如 `src/styles/tokens.css`），作为唯一事实来源：

```css
:root {
  /* ============ COLOR · LIGHT ============ */

  /* Surface — 纸张层级 */
  --surface:         oklch(94.5% 0.018 78);  /* 大麻纸 · 页面底 */
  --surface-raised:  oklch(92% 0.022 76);    /* 抬升卡片 · 气泡 · code bg */
  --surface-sunken:  oklch(88% 0.028 74);    /* avatar 空位 · 最深纸 */

  /* Content — 墨色文本层级 */
  --content:         oklch(22% 0.035 255);   /* 墨蓝近黑 · 正文 */
  --content-muted:   oklch(44% 0.028 255);   /* 元信息 · 次要标签 */
  --content-soft:    oklch(62% 0.020 250);   /* 占位 · 最弱辅助文本 */

  /* Signal — 主信号（赭红 Sienna） */
  --signal:          oklch(55% 0.13 42);     /* 主按钮 · agent 身份强调 */
  --signal-tint:     oklch(89% 0.035 50);    /* 背景标签 · 高亮 */
  --signal-deep:     oklch(40% 0.12 38);     /* hover/active · 深色文本 */

  /* Accent — 次信号（墨绿 Moss） */
  --accent:          oklch(44% 0.08 150);    /* 运行中 · 成功 */
  --accent-tint:     oklch(88% 0.038 145);   /* 次按钮背景 · 成功徽章 */
  --accent-deep:     oklch(30% 0.06 150);    /* 深色辅助 */

  /* Rule — 分隔线 */
  --rule:            oklch(72% 0.018 75);
  --rule-soft:       oklch(84% 0.015 75);

  /* ============ SPACING · 4→96 九阶 ============ */
  --space-1:  4px;
  --space-2:  8px;
  --space-3: 12px;
  --space-4: 16px;
  --space-5: 24px;
  --space-6: 32px;
  --space-7: 48px;
  --space-8: 64px;
  --space-9: 96px;

  /* ============ RADIUS · 0 / 4 / 8 / 12 ============ */
  --radius-0:  0;       /* rule 线容器 · 表格 · code */
  --radius-sm: 4px;     /* 徽章 · 输入 · tag */
  --radius-md: 8px;     /* 主圆角 · 按钮 · 卡片 · tool 块 · message 块 */
  --radius-lg: 12px;    /* 大容器 · pull-quote · orangutan placeholder */

  /* ============ SHADOW · 浅油墨 · 永不浓重 ============ */
  --shadow-0: none;
  --shadow-1: 0 1px 0 oklch(22% 0.035 255 / .05),
              0 1px 2px oklch(22% 0.035 255 / .04);
  --shadow-2: 0 2px 4px oklch(22% 0.035 255 / .06),
              0 1px 0 oklch(22% 0.035 255 / .05);

  /* ============ TYPOGRAPHY · 见 typography.md ============ */
  --font-display: 'Newsreader', Georgia, 'Times New Roman', serif;
  --font-body:    'Newsreader', Georgia, 'Times New Roman', serif;
  --font-mono:    'Spline Sans Mono', ui-monospace, 'SFMono-Regular',
                  Menlo, Consolas, monospace;

  --text-caption:  12px;
  --text-meta:     13px;
  --text-ui:       14px;
  --text-body:     17px;
  --text-lede:     20px;
  --text-h3:       24px;
  --text-h2:       32px;
  --text-h1:       48px;
  --text-masthead: 68px;

  /* ============ MOTION ============ */
  --ease:          cubic-bezier(0.2, 0.7, 0.2, 1);
  --duration-fast: 120ms;
  --duration-med:  200ms;
  --duration-slow: 320ms;

  /* ============ Z-STACK ============ */
  --z-base:    0;
  --z-sticky:  10;
  --z-overlay: 50;
  --z-modal:   100;
  --z-theme-sweep: 200;  /* 主题切换圆圈动画层 */
}

[data-theme="dark"] {
  /* ============ COLOR · DARK · 同家族非反相 ============ */

  --surface:         oklch(20% 0.028 260);   /* 夜墨 */
  --surface-raised:  oklch(26% 0.025 260);   /* 卷轴抬升 */
  --surface-sunken:  oklch(16% 0.025 260);

  --content:         oklch(92% 0.020 80);    /* 骨色 · 正文 */
  --content-muted:   oklch(70% 0.015 80);
  --content-soft:    oklch(52% 0.012 75);

  /* Sienna 在暗色下更亮暖 */
  --signal:          oklch(68% 0.14 42);
  --signal-tint:     oklch(32% 0.08 40);
  --signal-deep:     oklch(82% 0.10 45);

  /* Moss 在暗色下更饱和 */
  --accent:          oklch(60% 0.10 150);
  --accent-tint:     oklch(28% 0.06 150);
  --accent-deep:     oklch(78% 0.09 145);

  --rule:            oklch(36% 0.02 260);
  --rule-soft:       oklch(28% 0.02 260);

  /* shadow 深色下更贴近阴刻 */
  --shadow-1: 0 1px 0 oklch(0% 0 0 / .24),
              0 1px 2px oklch(0% 0 0 / .32);
  --shadow-2: 0 2px 4px oklch(0% 0 0 / .28),
              0 1px 0 oklch(0% 0 0 / .20);
}
```

---

## 2 · 色板语义规则

### Surface — 选哪一层？

| 令牌 | 什么时候用 |
|---|---|
| `--surface` | 页面底色 · chat 滚动区背景 |
| `--surface-raised` | 卡片 · 气泡 · `<code>` 背景 · tool-block 背景 · pull-quote 背景 |
| `--surface-sunken` | avatar 背景 · 被禁用输入 · 嵌套深一层的卡片 |

### Content — 文本该多浓？

| 令牌 | 用途 |
|---|---|
| `--content` | 所有正文 / 标题 / 主要可读文本 |
| `--content-muted` | 时间戳 · agent-key · role 徽章 · 辅助说明 · 表头 |
| `--content-soft` | placeholder · diff gutter · 极弱提示 |

### Signal（赭红） vs Accent（墨绿） — 不能混用

| 语义 | 用哪个 | 示例 |
|---|---|---|
| 主身份 / lead agent 强调 / 主 CTA | `--signal` | 主按钮、agent 名字斜体高亮 |
| 驳回 / 警告 / 待审批 / 删除行 | `--signal-tint` 背景 + `--signal-deep` 文字 | `badge.sienna`、diff `.del` |
| 成功 / 进行中 / 已完成 | `--accent-tint` + `--accent-deep` | `badge.moss`、diff `.add` |
| 一般状态徽章 | 中性，用 `--surface-raised` + `--content-muted` | `badge` 默认 |

**绝对不要：**把 Signal 用于"成功/已完成"，或把 Accent 用于"警告/驳回"——这会让气质错位。

---

## 3 · 透明 / 颜色混合

需要生成"半透明叠加"时使用 `color-mix(in oklch, ...)`：

```css
/* 正确：在 oklch 颜色空间混合 */
background: color-mix(in oklch, var(--signal-tint) 85%, transparent);

/* 禁止：直接用 rgba 硬编码 */
background: rgba(230, 156, 112, 0.85); /* ❌ 脱离令牌系统 */
```

用 `oklch(from ... l c h / alpha)` 做 alpha 变化：

```css
box-shadow: 0 0 0 3px oklch(from var(--signal) l c h / .15);
```

---

## 4 · 间距的语义配对

| 用途 | 令牌 |
|---|---|
| icon 与 label 之间 | `--space-2` (8) |
| label 与控件之间 | `--space-2` (8) |
| 输入控件上下内边距 | `10px` 纵 / `--space-3` 横 |
| 消息块内部段落间 | `--space-2` (8) |
| 消息块之间 | `--space-4` (16) 纵 + 虚线分隔 |
| 章节之间 | `--space-7` (48) 纵 + 实线 |
| 页面主区左右内边距 | `--space-5` 至 `--space-6` |
| 首屏 masthead 上下 | `--space-7` 至 `--space-8` |

---

## 5 · 圆角的语义

| 用途 | 令牌 |
|---|---|
| 代码块 · 表格 · 被 rule 包裹的内容 | `--radius-0` |
| 徽章 · 输入框 · 小 tag | `--radius-sm` (4) |
| 按钮 · 主卡片 · tool 块 · message 块 · reply-quote · thinking 块 | `--radius-md` (8) |
| 外层大容器 · pull-quote · orangutan placeholder · dark-preview | `--radius-lg` (12) |

---

## 6 · 阴影的语义

| 层级 | 用途 | 令牌 |
|---|---|---|
| 0 | 纸面平贴 · 用 rule 分隔 | `--shadow-0` |
| 1 | 常规抬升（卡片 · 输入 · 按钮 · tool 块） | `--shadow-1` |
| 2 | 悬浮（dialog · popover · 主题切换中层） | `--shadow-2` |

**永不：**再增加 shadow-3+ 的"飞起"阴影；这不是 material design。

---

## 7 · 完整色板速查（打印友好）

```
LIGHT
  paper  94.5% .018 78°    ink    22% .035 255°
  paper2 92%   .022 76°    muted  44% .028 255°
  paper3 88%   .028 74°    soft   62% .020 250°
  sienna 55%   .13  42°    moss   44% .08  150°
  s-tint 89%   .035 50°    m-tint 88% .038 145°
  s-deep 40%   .12  38°    m-deep 30% .06  150°
  rule   72%   .018 75°    rule-s 84% .015 75°

DARK
  paper  20%   .028 260°   ink    92% .020 80°
  paper2 26%   .025 260°   muted  70% .015 80°
  paper3 16%   .025 260°   soft   52% .012 75°
  sienna 68%   .14  42°    moss   60% .10  150°
  s-tint 32%   .08  40°    m-tint 28% .06  150°
  s-deep 82%   .10  45°    m-deep 78% .09  145°
  rule   36%   .02  260°   rule-s 28% .02  260°
```
