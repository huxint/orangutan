# 字体系统 · Typography

> 单家族策略：所有衬线文本都是 **Newsreader**，所有等宽都是 **Spline Sans Mono**。
> 层级靠 weight / italic / opsz / size 做，不引入第三种字体。

---

## 1 · 字体加载（CDN）

在 HTML 的 `<head>` 注入以下两行，**不得更改**：

```html
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Newsreader:ital,opsz,wght@0,6..72,300;0,6..72,400;0,6..72,500;0,6..72,600;0,6..72,700;1,6..72,300;1,6..72,400;1,6..72,500;1,6..72,600&family=Spline+Sans+Mono:ital,wght@0,400;0,500;0,600;1,400&display=swap" rel="stylesheet">
```

**原因解释（供未来维护者）：**
- `opsz 6..72` 的可变光学尺寸：Newsreader 在小号（opsz 接近 6）会显得更结实，大号（opsz 接近 72）会显得更优雅——浏览器会根据 CSS font-size 自动匹配。这对"同一家族扛起所有层级"至关重要。
- 斜体 wght 300-600 + 正体 wght 300-700：斜体用于 display / agent 名称 / thinking 块；正体 weight 400 做 body、500 做 label、600 做强调。

---

## 2 · fallback 链（永远附带）

```css
--font-body: 'Newsreader', Georgia, 'Times New Roman', serif;
--font-mono: 'Spline Sans Mono', ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace;
```

Google Fonts 加载失败时 Georgia 最接近 Newsreader 的字宽 / 对比度。

---

## 3 · 全局 base 规则

```css
html, body {
  font-family: var(--font-body);
  font-size: var(--text-body);       /* 17px */
  line-height: 1.62;
  text-wrap: pretty;                 /* 避免孤行、改善断行美学 */
  font-feature-settings: 'kern', 'liga', 'calt';
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
  color: var(--content);
  background: var(--surface);
}

/* 等宽默认特性：连字关闭，数字表格对齐 */
code, pre, .mono, [class*="mono"] {
  font-family: var(--font-mono);
  font-feature-settings: 'tnum', 'zero';
  letter-spacing: -.002em;            /* Spline Sans Mono 在 13px 下略需收紧 */
}
```

**硬约束：**`text-wrap: pretty` 是默认打开，不要在任何地方关掉。

---

## 4 · 字号阶梯

| 令牌 | 值 | 用途 | line-height | weight | style | letter-spacing |
|---|---|---|---|---|---|---|
| `--text-caption` | 12px | 表单小提示、swatch 值、footer | 1.5 | 400 | 正 | .02em |
| `--text-meta` | 13px | 时间戳、agent-key、元信息、tool head | 1.55 | 400 | 正 | .02em |
| `--text-ui` | 14px | 按钮、徽章、小段正文、label 配文 | 1.55 | 500 | 正 | -.005em |
| `--text-body` | 17px | 消息正文、段落、列表 | 1.62 | 400 | 正 | 0 |
| `--text-lede` | 20px | hero 子标、章节导语、dialog 头 | 1.5 | 400 | 斜 | -.005em |
| `--text-h3` | 24px | 侧栏标题、dialog heading | 1.35 | 500 | 斜 | -.01em |
| `--text-h2` | 32px | 章节标题、页面主区标题 | 1.2 | 500 | 斜 | -.012em |
| `--text-h1` | 48px | 页面 display、空状态大标题 | 1.1 | 600 | 斜 | -.018em |
| `--text-masthead` | 68px | 首屏 masthead / 品牌 wordmark | 1.04 | 600 | 斜 | -.02em |

---

## 5 · 层级与语气映射

### Display（斜体 · 承担身份感）
- **何时用：**章节标题、侧栏标题、空状态、masthead、agent 名字（显眼处）、pull-quote
- **字重：**500-600
- **斜体：**必须

```css
.display {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  letter-spacing: -.01em;
}
```

### Body（正体 · 承担阅读）
- **何时用：**消息正文、段落、长文
- **字重：**400
- **斜体：**禁用（仅内嵌 `<em>` 时）

### Label / UI text（中等字重）
- **何时用：**按钮文字、徽章、field label、列表项小字
- **字重：**500
- **字距：**负 0.005em（sans-ish 的紧凑）
- **字形：**一般正体；label 可用 uppercase + letter-spacing .1em（mono 才这么做）

### Mono（Spline Sans Mono · 承担结构）
- **何时用：**code、diff、key、tool 名、时间戳、session-id、命令、徽章文字（11-13px）
- **字重：**400 为主、500 用于 name / 关键词
- **字距：**-0.01em（13px 下）

---

## 6 · 特殊字号情形

### 小写英文字母缩写
时间戳、枚举、模型名（`claude-sonnet-4`）、键名（`ctrl+k`）：
- 统一使用 **Spline Sans Mono · 13px · weight 400 · 小写**
- 不要 uppercase · 不要 tracking

### uppercase 元信息标签（"RUNNING"、"ERROR"、"§ 01"）
- 仅在 **caption（12px）或 meta（13px）且是 mono** 场景使用
- `text-transform: uppercase`
- `letter-spacing: .08em-.12em`（小号才加字距）
- weight 500

### agent 名字（"lead"、"research"）在消息头部
- 字体：**Newsreader 斜体 · 16px · weight 600 · letter-spacing 0**
- 颜色：`--content`（本身不染色；用旁边的角色徽章染色）
- 保留斜体是 V2 的手稿灵魂

### agent "role" 小字（"主协调" / "委派"）
- 字体：**Newsreader 斜体 · 14px · weight 400**
- 颜色：`--signal-deep`（Sienna 深）
- 承担"agent 身份"的视觉锚点

---

## 7 · 数学排版（预留）

数学公式由 KaTeX 渲染，字体不使用 Newsreader 的 glyph 做近似，而是让 KaTeX 自带 `KaTeX_Main` 等字体。**但行内 / 块级 math 的包裹容器**（`.math-inline` / `.math-block`）应：

```css
.math-inline {
  font-family: 'Newsreader', serif;   /* 对齐气质，KaTeX 渲染时内部会替换 */
  font-style: italic;
  font-size: 1.04em;
  padding: 0 3px;
}

.math-block {
  font-family: 'Newsreader', serif;
  font-size: 22px;                    /* 显式 22px，不用 em */
  text-align: center;
  padding: var(--space-6) var(--space-5);
  background: var(--surface);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
  letter-spacing: .01em;
}
```

数学块上方用 mono 小 tag 标示 "Delimited Block Math · 规范化后"。

Math 规范化由**消息块步骤（Step C）**实现，本步骤只定义视觉容器。

---

## 8 · 排版节奏（垂直韵律）

- 段落间距：`--space-3`（12px）——消息正文紧凑但透气
- 列表项间距：`--space-2`（8px）
- 章节标题上下：`--space-5` 上 · `--space-2` 下（标题和正文紧贴，和上一区块留距）
- masthead 装饰 rule 线：距标题 `--space-4`，宽度 100px，颜色 `--content`

---

## 9 · 禁止事项

- ❌ 在项目任何地方出现 **Inter / Roboto / Arial / Fraunces / system-ui** 字体族（参见 guardrails.md 完整清单）
- ❌ 用 `font-weight: bold` 或 `<strong>` 做强调时切换字体（始终是 Newsreader 内的 600）
- ❌ 在 sans 场景加斜体（Spline Sans Mono 有斜体变体，但**仅用于 code 里的占位符参数**，如 `<filename>`）
- ❌ 任意调整 `letter-spacing` 到超过上表规定的范围
- ❌ 在正文中用 uppercase 长文（只允许 ≤4 词的短标签）

---

## 10 · 自检

在任何实现 PR 中，抓几段随机文本截图，确认：
- [ ] 消息正文是 Newsreader 17px 正体
- [ ] agent 名字是 Newsreader 16px 斜体 · weight 600
- [ ] 时间戳是 Spline Sans Mono 13px
- [ ] 章节标题是 Newsreader 32px 斜体 · weight 500
- [ ] 没有任何位置出现 Inter / system-ui / sans-serif 的直接调用
