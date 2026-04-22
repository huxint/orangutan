# Text & Markdown · 文本块与 markdown 渲染

> 消息正文的渲染规格。大部分 agent 输出是自由 markdown，但"自由"不是"混乱"——**规范化掉不合手稿气质的转换**（花体引号、强调底色、bullet 字符），让 Newsreader 的阅读感贯穿。
> 本文件覆盖：段落 · inline code · emphasis · 列表 · 表格 · 标题 · 块引用 · 分隔线 · 链接 · 内嵌图。

---

## 1 · `.msg__body` 基线 · 不复述 Step 01

基础样式已在 [`01-visual-language/components.md § 5`](../01-visual-language/components.md) 锁定。本步骤**只补充**以下在 markdown 渲染中容易被 LLM 输出打破的边界：

```css
.msg__body {
  font-family: var(--font-body);
  font-size: var(--text-body);       /* 17px */
  line-height: 1.62;
  color: var(--content);
  text-wrap: pretty;                  /* 禁改 */
  overflow-wrap: anywhere;            /* 长英文/URL 不撑出气泡 */
}

.msg__body > *:first-child { margin-top: 0; }
.msg__body > *:last-child  { margin-bottom: 0; }
```

**所有段落 / 列表 / 表格 / 块引用 / 分隔线最大宽度 64ch**，通过 `max-width: 64ch` 施加在每个直接子元素上。代码块、diff 块、math 块、image 块**例外**——它们有自己的宽度策略（见各自文档）。

---

## 2 · 推荐 markdown 处理管线

```
[LLM 输出 string]
   ↓ 1) 规范化 math 界定符（→ math.md § 2）
   ↓ 2) 规范化 smart quotes / em-dash（见本文 § 3）
   ↓ 3) parse（推荐 unified + remark-parse + remark-gfm）
   ↓ 4) 语义转换（自定义 plugin：见本文 § 4）
   ↓ 5) rehype（HTML AST）
   ↓ 6) sanitize（白名单 tag + attr，见 § 11）
   ↓ 7) stringify to HTML
   ↓ 8) 挂载到 .msg__body · innerHTML
```

**推荐库**：
- [`remark-parse`](https://github.com/remarkjs/remark) + [`remark-gfm`](https://github.com/remarkjs/remark-gfm)（表格、任务列表、删除线）
- [`rehype-raw`](https://github.com/rehypejs/rehype-raw)（若允许 inline HTML）
- [`rehype-sanitize`](https://github.com/rehypejs/rehype-sanitize)（白名单）
- [`rehype-katex`](https://github.com/remarkjs/remark-math)（math · 见 `math.md`）
- [`shiki`](https://shiki.style) 或 [`highlight.js`](https://highlightjs.org)（code · 见 `code-and-diff.md`）

**禁用**：
- ❌ `SmartyPants` / `smartypants`（会把 `"中文"` 和英文引号错误转换）
- ❌ `remark-squeeze-paragraphs`（段落意图是内容信号，不要合并）

---

## 3 · 标点与字形规范化（非 markdown · agent 输出前置步骤）

LLM 可能输出各种引号 / em-dash / 星号等。统一做如下替换（**在 parse markdown 之前**，纯字符串替换）：

| 输入 | 保留 / 改为 | 原因 |
|---|---|---|
| `“”` / `"" `直引号混用 | **保留原样** | 不要 SmartyPants 式转换 · 中文引号如 `「」` `『』` 也原样保留 |
| `--` 连续两短横 | 保留为 `--` · 不转 `—` | 除非前后都是空格（即 `text -- text`）才转 em-dash |
| ` - ` 空格包围的单短横 | 保留 `-` | 不转 bullet、不转 em-dash |
| `...` 三点 | 保留 | 不转 `…`（会影响等宽对齐） |
| 裸 URL（不含 angle brackets） | 自动转成 `<a href>` | 见 § 9 链接 |
| emoji（`😀` 等） | **保留原样**，不删 | 用户消息里可能有，agent 少用 |
| 半角/全角数字混用 | 保留原样 | 不做转换 |

**规范化函数伪代码**：

```js
function normalizeText(md) {
  return md
    // em-dash 仅在空格包围时生效
    .replace(/(\s)--(\s)/g, '$1—$2')
    // 不做任何引号转换
    // 不做任何 ellipsis 转换
    // autolink 裸 URL
    .replace(/(^|\s)(https?:\/\/[^\s<]+)/g, '$1<$2>');
}
```

---

## 4 · Markdown 元素映射表

| markdown | HTML | 样式类 | 要点 |
|---|---|---|---|
| `段落` | `<p>` | `.msg__body > p` | `max-width: 64ch`，段距 `--space-3` |
| `*em*` / `_em_` | `<em>` | — | italic Newsreader 内联，**禁用** font-weight 或底色 |
| `**strong**` / `__strong__` | `<strong>` | — | `font-weight: 600`，**禁用** 底色高亮或彩字 |
| `` `code` `` | `<code>` | `.msg__body p code` | 见 § 5 inline code |
| `~~strike~~`（GFM） | `<del>` | — | `text-decoration: line-through; color: var(--content-soft)` |
| `[text](url)` | `<a>` | `.msg__body a` | 见 § 9 链接 |
| `# h1` — `###### h6` | `<h1>` — `<h6>` | `.msg__body h{1-6}` | 见 § 6 标题 |
| `- item` / `* item` | `<ul><li>` | `.msg__body ul` | 见 § 7 列表 |
| `1. item` | `<ol><li>` | `.msg__body ol` | 见 § 7 |
| `- [ ]` / `- [x]`（GFM） | `<ul class="task"><li>` | `.msg__body ul.task` | 见 § 7 任务列表 |
| `> quote` | `<blockquote>` | `.msg__body blockquote` | 见 § 8 块引用 |
| `---` / `***` | `<hr>` | `.msg__body hr` | 见 § 10 分隔线 |
| `` ``` `` 代码块 | `<pre><code>` | → `code-and-diff.md` | **不在本文件**（block 级） |
| `$$…$$` 块 math | → `math-block` | → `math.md` | **不在本文件** |
| 表格（GFM） | `<table>` | `.msg__body table` | 见 § 7 |

**未在表中出现的 markdown**（如 `<u>`、`<sub>`、`<sup>`）**禁止支持**，防止 LLM 输出样式失控。

---

## 5 · Inline code · 已在 Step 01 · 本节只强化两条

```css
.msg__body p code,
.msg__body li code,
.msg__body td code,
.msg__body th code {
  font-family: var(--font-mono);
  font-size: .88em;
  background: var(--surface-raised);
  padding: 1px 6px;
  border-radius: var(--radius-sm);
  color: var(--content);
  /* 防止长标识符撑破气泡 */
  word-break: break-word;
  overflow-wrap: anywhere;
}
```

**规则**：
- inline code 在 `<a>` 里时保留背景（视觉上是链接的一部分），但链接的下划线**不加在 code 上**（`a code { text-decoration: none; }`）
- inline code 在 `<strong>` 里时不叠加字重（已经是 mono，不加粗）

---

## 6 · 标题 · h1–h6 · 压低重量

Agent 的正文很少会主动写 h1；大多数是 h3-h4 分节。我们**把 h1-h2 压到接近章节感**，避免"大标题横贯屏幕"的 AI-slop 感。

```css
.msg__body h1 {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 24px;
  letter-spacing: -.01em;
  line-height: 1.3;
  margin: var(--space-6) 0 var(--space-3);
  color: var(--content);
  max-width: 64ch;
}
.msg__body h2 {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 20px;
  letter-spacing: -.008em;
  line-height: 1.3;
  margin: var(--space-5) 0 var(--space-3);
  color: var(--content);
  max-width: 64ch;
}
.msg__body h3 {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 17px;       /* 与正文同号，只靠 italic + weight 做层级 */
  margin: var(--space-5) 0 var(--space-2);
  color: var(--content);
  max-width: 64ch;
}
.msg__body h4,
.msg__body h5,
.msg__body h6 {
  font-family: var(--font-mono);
  font-size: 12px;
  text-transform: uppercase;
  letter-spacing: .12em;
  font-weight: 500;
  color: var(--content-muted);
  margin: var(--space-4) 0 var(--space-2);
  max-width: 64ch;
}
```

**为什么压低** —— `.msg__body` 本身 17px，h1 48px 会盖过消息本身变成"文章标题"。把 h1 压到 24px、h3 归到 17px，让消息感不丢失。

---

## 7 · 列表与表格

### 无序列表

```css
.msg__body ul {
  margin: var(--space-3) 0;
  padding-left: var(--space-5);
  max-width: 64ch;
}
.msg__body ul > li {
  margin: var(--space-2) 0;
  /* 自定义 marker：短 em-dash，不用 • 或 ◦ */
}
.msg__body ul > li::marker {
  content: '— ';
  color: var(--content-soft);
}
```

**禁止**：
- ❌ 用 emoji 或表情符做 marker（`🔸` `✨` `✅` 等）
- ❌ 自定义 SVG bullet · 保持纯文字 marker

### 有序列表

```css
.msg__body ol {
  margin: var(--space-3) 0;
  padding-left: var(--space-5);
  max-width: 64ch;
  font-variant-numeric: tabular-nums; /* 数字对齐 */
}
.msg__body ol > li {
  margin: var(--space-2) 0;
}
.msg__body ol > li::marker {
  font-family: var(--font-mono);
  font-size: .92em;
  color: var(--content-muted);
}
```

### 任务列表（GFM）

```html
<ul class="task">
  <li><input type="checkbox" disabled> 未完成项</li>
  <li><input type="checkbox" disabled checked> 已完成项</li>
</ul>
```

```css
.msg__body ul.task { list-style: none; padding-left: var(--space-4); }
.msg__body ul.task > li {
  display: grid;
  grid-template-columns: 18px 1fr;
  gap: var(--space-3);
  align-items: start;
}
.msg__body ul.task input[type="checkbox"] {
  /* 重置原生 */
  appearance: none;
  width: 14px;
  height: 14px;
  border: 1.4px solid var(--rule);
  border-radius: 3px;
  margin-top: 5px;
  cursor: default;
  background: var(--surface);
  position: relative;
}
.msg__body ul.task input[type="checkbox"]:checked {
  background: var(--accent-tint);
  border-color: var(--accent);
}
.msg__body ul.task input[type="checkbox"]:checked::after {
  content: '';
  position: absolute;
  inset: 1px;
  background-image:
    linear-gradient(45deg, transparent 40%,
                          var(--accent-deep) 40%,
                          var(--accent-deep) 45%, transparent 45%),
    linear-gradient(-45deg, transparent 55%,
                           var(--accent-deep) 55%,
                           var(--accent-deep) 65%, transparent 65%);
  background-size: 60% 60%;
  background-position: center 55%;
  background-repeat: no-repeat;
}
```

### 嵌套列表

嵌套 >3 层时 console.warn，视为 markdown 输入错误——大部分 agent 不需要 >3 层。

### 表格

```css
.msg__body table {
  margin: var(--space-4) 0;
  border-collapse: collapse;
  width: auto;
  max-width: 100%;
  font-size: var(--text-ui);
  font-variant-numeric: tabular-nums;
  color: var(--content);
}
.msg__body table thead th {
  text-align: left;
  font-family: var(--font-mono);
  font-size: 11px;
  text-transform: uppercase;
  letter-spacing: .1em;
  color: var(--content-muted);
  padding: var(--space-2) var(--space-4);
  border-bottom: 1px solid var(--rule);
  white-space: nowrap;
}
.msg__body table td {
  padding: var(--space-2) var(--space-4);
  border-bottom: 1px solid var(--rule-soft);
  vertical-align: top;
}
.msg__body table tr:last-child td { border-bottom: none; }
.msg__body table td code { font-size: .9em; }
```

**宽表处理**：父容器加 `overflow-x: auto`。Agent 不太会输出超宽表；若出现，用户横向滚动即可。

---

## 8 · 块引用 · blockquote · 手稿式缩进，不加彩色竖条

Agent 偶尔用 `>` 引用用户的话或文档原文。**禁止**用"圆角 + 左 signal 色竖条"的 AI-slop 风；我们用**缩进 + italic + 薄 left hairline（中性色）**：

```css
.msg__body blockquote {
  margin: var(--space-4) 0;
  padding: 0 var(--space-4);
  border-left: 2px solid var(--rule);     /* 中性色 · 非 signal · OK */
  color: var(--content-muted);
  font-style: italic;
  font-size: var(--text-ui);
  line-height: 1.6;
  max-width: 60ch;
}
.msg__body blockquote p { margin: var(--space-2) 0; }
.msg__body blockquote > :first-child { margin-top: 0; }
.msg__body blockquote > :last-child  { margin-bottom: 0; }
```

**border-left 2px 中性 rule** —— 不是 Sienna / Moss / 任何信号色。这一条 hairline 是"手稿里的小注记标"，不是 AI-slop 的"accent bar"。

**嵌套块引用**（`>>`）：缩进再加一次 border-left——不换颜色、不换宽度。

---

## 9 · 链接 · 下划线式，hover signal-deep

```css
.msg__body a {
  color: var(--content);
  text-decoration: underline;
  text-decoration-color: var(--rule);
  text-decoration-thickness: 1px;
  text-underline-offset: 2.5px;
  transition: color var(--duration-fast) var(--ease),
              text-decoration-color var(--duration-fast) var(--ease);
}
.msg__body a:hover {
  color: var(--signal-deep);
  text-decoration-color: var(--signal);
}
.msg__body a:focus-visible {
  outline: 2px solid var(--signal);
  outline-offset: 2px;
}
.msg__body a code { text-decoration: none; }
```

**外部链接额外视觉提示**（可选）：在 `href` 为 `http(s)://` 且非当前 host 时，追加一个 12px 的外链图标（Phosphor `ArrowUpRight`）：

```css
.msg__body a[href^="http"][target="_blank"]::after {
  content: '';
  display: inline-block;
  width: 10px; height: 10px;
  margin-left: 3px;
  background: currentColor;
  mask: url('/icons/external.svg') no-repeat center / contain;
}
```

---

## 10 · 水平分隔线 · hr

```css
.msg__body hr {
  margin: var(--space-5) 0;
  border: none;
  height: 1px;
  background: var(--rule-soft);
  max-width: 64ch;
}
```

**禁止**：
- ❌ 渐变 hr
- ❌ 装饰花边 hr（`~~~~` 的 ASCII 艺术）

---

## 11 · HTML 安全 · sanitize 白名单

Agent 可能输出内联 HTML。接入 [`rehype-sanitize`](https://github.com/rehypejs/rehype-sanitize)，白名单：

```js
const schema = {
  tagNames: [
    'a', 'abbr', 'blockquote', 'br', 'code', 'del', 'em', 'hr',
    'h1', 'h2', 'h3', 'h4', 'h5', 'h6',
    'img', 'ins', 'kbd', 'li', 'mark', 'ol', 'p', 'pre', 's',
    'strong', 'sub', 'sup', 'table', 'tbody', 'td', 'th', 'thead', 'tr', 'ul',
    // math 包裹由 rehype-katex 注入
    'span', 'div',
  ],
  attributes: {
    a: ['href', 'title', 'target', 'rel'],
    img: ['src', 'alt', 'title', 'width', 'height'],
    code: ['className'],
    pre: ['className'],
    span: ['className'],
    div: ['className', 'dataView'],
    th: ['align'], td: ['align'],
    input: ['type', 'checked', 'disabled'],
  },
  protocols: { href: ['http', 'https', 'mailto', '#'], src: ['http', 'https', 'data'] },
};
```

**强制**：所有 `a` 加 `rel="noopener noreferrer"` · 外链 `target="_blank"`。

---

## 12 · 行内图 · 小图嵌在段落内

罕见但存在（比如 agent 粘贴一个头像引用或小截图）。当 `<img>` 出现在 `<p>` 里：

```css
.msg__body p img {
  max-height: 1.4em;
  vertical-align: -.2em;
  display: inline-block;
  margin: 0 2px;
}
```

**块级图**（独立一行、非 inline）走 `specialty-blocks.md · § 4` 的 Image Block。

---

## 13 · 自检

- [ ] `.msg__body > *` 都有 `max-width: 64ch`（除 code/diff/math/image block）
- [ ] `text-wrap: pretty` 未被覆盖
- [ ] `h1-h3` 是 italic Newsreader，`h4-h6` 是 uppercase mono
- [ ] 列表 marker 是 `—` 或 mono 数字，**不是** emoji / SVG
- [ ] 块引用有中性 `--rule` 左 hairline，**不是** signal 色
- [ ] 链接下划线 rule-color，hover signal-deep
- [ ] SmartyPants / 引号转换**已禁用**
- [ ] sanitize 白名单覆盖所有必要 tag，剔除 `<script>` `<style>` `<iframe>` `<object>` `<embed>`
