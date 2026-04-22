# Math · 公式规范化与 KaTeX 渲染

> LLM 输出的 math 界定符**非常凌乱**——同一条消息里可能既有 `$$…$$`、又有 `\[…\]` 和 `\(…\)`、偶尔还有单 `$…$` inline。
> 本文件定义**规范化规则**（normalize 到统一的内部表示）、**KaTeX 渲染管线**、以及**wrapper 视觉**。

---

## 1 · 三种输入界定符 · 按输入形式命名

| 输入形式 | 名字 | 语义 | 目标 |
|---|---|---|---|
| `$$ … $$` | **DollarBlock** | 块级公式（display math） | 渲染为独立 block，居中 |
| `\[ … \]` | **DelimitedBlock** | 块级公式（display math · LaTeX 经典） | 同上 · 与 DollarBlock 等价 |
| `\( … \)` | **DelimitedInline** | 行内公式（inline math） | 内联在 `<p>` 中 |
| `$ … $` | DollarInline（补充） | 行内公式 · 单 `$` 包裹 | 同 DelimitedInline |

**规范化目标**：所有 3（或 4）种输入形式**统一为两种内部形式**：
- `$$ … $$` → block math
- `$ … $` → inline math

这样下游 markdown parser（`remark-math`）只需处理两种情形。

---

## 2 · 规范化管线 · parse markdown 前的预处理

**必须在 markdown parser 之前执行**，因为原始的 markdown 规范不认识 `\(...\)` / `\[...\]`，会错解。

```js
function normalizeMath(text) {
  // Step 0: 保护 code 块和 code span, 不碰里面的 $ 和 \
  const codeFencePattern = /```[\s\S]*?```/g;
  const codeSpanPattern  = /`[^`\n]+`/g;
  const inlineCodeReplacements = [];
  text = text.replace(codeFencePattern, (m) => {
    const token = `___CODE_${inlineCodeReplacements.length}___`;
    inlineCodeReplacements.push(m);
    return token;
  });
  text = text.replace(codeSpanPattern, (m) => {
    const token = `___ICODE_${inlineCodeReplacements.length}___`;
    inlineCodeReplacements.push(m);
    return token;
  });

  // Step 1: \[ … \] → $$ … $$
  text = text.replace(/\\\[([\s\S]+?)\\\]/g, (_, inner) => `$$${inner}$$`);

  // Step 2: \( … \) → $ … $
  text = text.replace(/\\\(([\s\S]+?)\\\)/g, (_, inner) => `$${inner}$`);

  // Step 3: DollarBlock 规范化：确保 $$...$$ 前后有换行（block 语义）
  text = text.replace(
    /(^|[^\$])\$\$([\s\S]+?)\$\$(?!\$)/g,
    (_, pre, inner) => `${pre}\n$$${inner.trim()}$$\n`,
  );

  // Step 4: DollarInline · 仅在两侧有非空白字符时才视为 math
  // · 防止 "cost is $5 a pop" 被当成 math (无配对 $)
  // · 简单约束：同一行内成对出现才算
  text = text.replace(
    /(^|[^\\\$])\$(?![\s])([^\$\n]+?)(?<![\s])\$(?!\d)/g,
    (_, pre, inner) => `${pre}$${inner}$`,
  );

  // Step 5: 还原 code 占位
  inlineCodeReplacements.forEach((code, i) => {
    text = text.replace(`___CODE_${i}___`, code).replace(`___ICODE_${i}___`, code);
  });

  return text;
}
```

**关键细节**：
- 保护 code 首要 —— agent 的代码里 `$` 和 `\` 合法，不能被规范化
- `\[ \]` 和 `\( \)` 的匹配是非贪婪（`[\s\S]+?`）
- `$$…$$` 标准化后**前后各加 `\n`**，让 markdown 识别为 block 而非 inline
- `$…$` inline 的"两侧非空白"约束防止 `$5` / `3 $` 之类误触发
- LaTeX 命令（`\frac` / `\sum` / `_` / `^`）在 dollar span 内部合法，不处理

---

## 3 · 启发式：`$X$` 是否真的是 math？

有些 agent 会写"cost is \$5"之类——虽然单 `$` 没配对，但万一 agent 恰好配对两个美元符号来描述钱呢？

**Rule**：若规范化后的 dollar inline span 满足以下任一，**视为 math**；否则撤销规范化，保留字面量：

- 内部包含 `\\[a-zA-Z]+` （LaTeX 命令，如 `\frac`、`\alpha`）
- 内部包含 `_` 或 `^` 且前后紧跟字母 / 数字
- 内部包含 `{` `}`
- 内部字符仅限 `[a-zA-Z0-9+\-*/=<>≤≥|() α-ω]`（希腊字母）

**代码实现**：在上述 Step 4 之后加一遍 post-filter：

```js
function isLikelyMath(inner) {
  return /\\[a-zA-Z]+/.test(inner)
      || /[\^_]\w/.test(inner)
      || /[{}]/.test(inner)
      || /^[a-zA-Z0-9+\-*/=<>≤≥|()\sα-ω]+$/.test(inner);
}

text = text.replace(/\$([^$\n]+?)\$/g, (full, inner) =>
  isLikelyMath(inner) ? full : full.replace(/^\$|\$$/g, '\\$')
);
```

---

## 4 · 渲染管线 · remark/rehype + KaTeX

```js
import { unified } from 'unified';
import remarkParse from 'remark-parse';
import remarkMath from 'remark-math';
import remarkGfm from 'remark-gfm';
import remarkRehype from 'remark-rehype';
import rehypeKatex from 'rehype-katex';
import rehypeSanitize from 'rehype-sanitize';
import rehypeStringify from 'rehype-stringify';

function renderMessage(raw) {
  const normalized = normalizeMath(raw);
  return unified()
    .use(remarkParse)
    .use(remarkGfm)
    .use(remarkMath)
    .use(remarkRehype, { allowDangerousHtml: true })
    .use(rehypeKatex, {
      output: 'htmlAndMathml',     // 屏幕阅读器用 MathML
      trust: false,                 // 禁用 \href / \includegraphics
      strict: 'ignore',             // 坏 LaTeX 降级为 warning，不抛
      macros: {
        '\\argmax': '\\mathop{\\mathrm{argmax}}',
        '\\argmin': '\\mathop{\\mathrm{argmin}}',
      },
    })
    .use(rehypeSanitize, katexSchema)  // 见 text-markdown.md § 11
    .use(rehypeStringify)
    .processSync(normalized)
    .toString();
}
```

**加载 KaTeX CSS**（必须）：
```html
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/katex@0.16/dist/katex.min.css">
```

但我们要**覆盖**几个 KaTeX 默认样式让它与 Newsreader 协调（见 § 6）。

---

## 5 · Block Math · 两种挂载形式

### 形式 A · 独立 block（推荐，默认）

公式独占一行，无 bar，只看见公式本体。

```html
<div class="math-block" data-normalized-from="DollarBlock">
  <!-- KaTeX 注入 · 含 .katex .katex-display -->
</div>
```

```css
.math-block {
  font-family: var(--font-display);
  font-size: 20px;
  line-height: 1.45;
  text-align: center;
  padding: var(--space-5) var(--space-4);
  background: var(--surface);
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
  color: var(--content);
  margin: var(--space-4) 0;
  overflow-x: auto;                 /* 超宽公式水平滚动，不撑破消息 */
}
```

### 形式 B · 带 bar（当需要元信息时）

用户/agent 在 UI 中显式要求"展示 LaTeX 源码"或"复制公式"时包一层 `.block`：

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="block__chip">math</span>
      <span class="block__name">预期命中率</span>
      <span class="block__meta">DollarBlock → Block <em>·</em> 规范化后 <em>·</em> KaTeX</span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="查看 LaTeX 源码"><!-- code icon --></button>
      <button class="block__btn" aria-label="复制 LaTeX"><!-- copy icon --></button>
    </div>
  </div>
  <div class="math-block">
    <!-- KaTeX 注入 -->
  </div>
</div>
```

**何时用 A vs B**：
- 默认 A（公式本体即所见）
- 当公式有引用意义（"式 (3)"）或 agent 要求"给出可复制 LaTeX"时用 B
- 一条消息里 A / B 都可能出现，不冲突

---

## 6 · KaTeX 样式覆写 · 与 Newsreader 协调

KaTeX 默认用自带 `KaTeX_Main` 等字体。我们**保留它们**（KaTeX 字体对数学字形的专业度无可替代），但做以下微调让公式与 Newsreader 的斜体变量不"打架"：

```css
.math-block .katex,
.math-inline .katex {
  font-size: inherit;             /* 交给容器决定 */
  color: inherit;
}

/* KaTeX 默认斜体变量（x, y, λ）用 KaTeX_Math · 保留 */
/* 但运算符和数字让其稍微收紧 letter-spacing */
.math-block .katex .mop,
.math-block .katex .mord.mathnormal { letter-spacing: 0; }

/* display math 的 .katex-display 不要强加上下 margin · 交给 .math-block padding */
.math-block .katex-display { margin: 0; }
```

**Dark mode 下**：KaTeX 的颜色会随 `color: inherit` 自动变；无需额外规则。

---

## 7 · Inline Math · 行内公式

```html
<p>
  其中 <span class="math-inline">λ</span> 是请求到达率
  （<span class="math-inline">\lambda</span>，真实渲染后变 λ）
</p>
```

```css
.math-inline {
  font-family: var(--font-display);  /* 容器字体；KaTeX 内部字体不受影响 */
  font-size: 1.04em;
  padding: 0 2px;
  color: var(--content);
  /* 防止行内公式撑高行 */
  vertical-align: baseline;
}
.math-inline .katex {
  font-size: inherit;
  line-height: 1;
}
```

**规则**：
- inline math **不换行**（`.katex` 内部默认 `display: inline-block`）
- 超宽 inline math（罕见）不处理——公式太长就应当 block 展示
- 不给 inline math 加背景高亮，保持阅读流畅

---

## 8 · Normalized-from 元信息（调试用）

生成 HTML 时，给 math 节点加 `data-normalized-from` 属性，记录原始界定符形式：

```html
<div class="math-block" data-normalized-from="DollarBlock">…</div>
<div class="math-block" data-normalized-from="DelimitedBlock">…</div>
<span class="math-inline" data-normalized-from="DelimitedInline">…</span>
<span class="math-inline" data-normalized-from="DollarInline">…</span>
```

**用途**：
- DevTools 调试数学 pipeline 时一眼看出来源
- 若未来要在 `.block` B 形式的 `.block__meta` 里显示来源，取这个值即可

---

## 9 · 错误处理 · LaTeX 失败的 fallback

KaTeX 解析失败时（`strict: 'ignore'` 下仍可能抛），我们**不显示 "rendered" 的错误红字**——那太刺目。而是显示**原始 LaTeX 源**在一个带 warning 的 `.math-block--err`：

```html
<div class="math-block math-block--err" title="LaTeX 解析失败">
  <pre class="math-raw">$$P(hit) = \frac{unmatched brace$$</pre>
  <span class="math-err-meta">rendering failed · showing source</span>
</div>
```

```css
.math-block--err {
  background: color-mix(in oklch, var(--signal-tint) 15%, var(--surface));
  border-color: oklch(from var(--signal) l c h / .35);
}
.math-block--err .math-raw {
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  color: var(--signal-deep);
  white-space: pre-wrap;
  margin: 0;
  text-align: left;
}
.math-block--err .math-err-meta {
  display: block;
  margin-top: var(--space-3);
  font-family: var(--font-mono);
  font-size: 11px;
  letter-spacing: .08em;
  text-transform: uppercase;
  color: var(--signal-deep);
  opacity: .7;
  text-align: left;
}
```

---

## 10 · 可访问性

- **MathML 输出**：`rehype-katex` 开 `output: 'htmlAndMathml'`，KaTeX 会同时输出视觉 HTML 和 MathML（后者给屏幕阅读器）
- **aria-label**：`.math-block` 加 `role="math"` + `aria-label`（从 `katex-html` 的文本提取，或由 agent 提供）
- **颜色对比**：block math 的前景色应 ≥ AA（`--content` on `--surface` 约 12:1，通过）
- **字号底线**：inline math 的 1.04em ≥ 14px 最小阈值（基础 17px × 1.04 > 17）

---

## 11 · 速查

| 问题 | 答案 |
|---|---|
| 三种 block 界定符（`$$` / `\[`）规范化目标？ | 都变 `$$…$$` |
| 三种 inline 界定符（`\(` / `$`）规范化目标？ | 都变 `$…$` |
| 代码块里的 `$` 怎么处理？ | 预处理时保护起来，规范化完再还原 |
| `cost is $5` 会误触发吗？ | 不会 · 单 `$` 无配对，且启发式会保留字面量 |
| 坏 LaTeX 怎么办？ | `.math-block--err` 显示源 + warning meta |
| 公式要加 `§ math` 标签吗？ | 默认**不加**（形式 A）；需要引用 / 复制时用形式 B |
| KaTeX CSS 要自己写吗？ | 不要 · 加载官方 `katex.min.css`，只覆盖 § 6 的少量规则 |

---

## 12 · 自检

- [ ] `\(...\)` 和 `\[...\]` 在渲染前已规范化为 `$...$` / `$$...$$`
- [ ] `$...$` 的启发式避免误识别（钱 / 代码 / 变量名）
- [ ] 代码块内 `$` 与 `\` 不被规范化
- [ ] block math `text-align: center`；inline math `vertical-align: baseline` 不撑行高
- [ ] KaTeX `output: 'htmlAndMathml'` 已启用
- [ ] LaTeX 失败时显示 `.math-block--err`，不空块、不红色 error
- [ ] 深色主题下公式颜色正确（inherit 生效）
- [ ] 超宽公式父容器 `overflow-x: auto`
