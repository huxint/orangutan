# Code & Diff · 代码块、块级 diff、行内 diff

> 代码与 diff 是 lead 与 teammate 的**主要可视产出**。它们的默认展开状态是 **open**，折叠只在极长（code > 80 行 / diff > 200 行）时触发。
> 本文件覆盖：代码块（语法高亮 / 行号 / chrome / 操作按钮）· 块 diff（unified + split 切换）· 行内 diff（三重冗余）。

---

## 1 · Code Block · HTML 结构

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="block__chip">cpp</span>
      <span class="block__name">src/agent/agent-loop.cpp</span>
      <span class="block__meta">L138–L148 <em>·</em> 11 行</span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="在编辑器打开">
        <svg class="icon"><!-- external-link --></svg>
      </button>
      <button class="block__btn" aria-label="复制代码">
        <svg class="icon"><!-- copy --></svg>
      </button>
    </div>
  </div>
  <div class="code-v3">
    <pre class="code-v3__gutter">138
139
140
141
142
143
144
145
146
147
148</pre>
    <pre class="code-v3__body"><span class="tok-com">// …</span>
<span class="tok-kw">while</span> (…) { … }</pre>
  </div>
</div>
```

**必守**：
- gutter 用 `<pre>` 不用 `<div>` —— 保证 `white-space: pre` 和等宽 line-height
- gutter 与 body 的 `font-size` + `line-height` **必须完全一致**（见 § 2）
- `.block__chip` 永远中性描边（不给语言染色）
- `.block__name` 显示**文件路径 or 标识符**（若 agent 只给了语言、无文件名，则 name 省略，chip 独立显示）
- `.block__meta` 格式：`L{start}–L{end} · N 行` 或 `N 行`（无行号信息时）

---

## 2 · Code Block · CSS

```css
.code-v3 {
  display: grid;
  grid-template-columns: 44px 1fr;
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  line-height: 1.72;
  overflow: hidden;           /* 内部 body 有 overflow-x */
  tab-size: 2;
  background: var(--surface);
}

.code-v3__gutter {
  padding: var(--space-4) var(--space-2) var(--space-4) var(--space-3);
  margin: 0;
  text-align: right;
  color: var(--content-soft);
  border-right: 1px solid var(--rule-soft);
  user-select: none;
  white-space: pre;
  font-family: var(--font-mono);
  font-size: var(--text-meta);      /* 必须与 body 一致 */
  line-height: 1.72;                 /* 必须与 body 一致 */
  background: var(--surface);
}

.code-v3__body {
  padding: var(--space-4) var(--space-5);
  margin: 0;
  white-space: pre;
  color: var(--content);
  overflow-x: auto;
  background: var(--surface);
  font-size: var(--text-meta);
  line-height: 1.72;
  font-feature-settings: 'tnum', 'zero';
  letter-spacing: -.002em;
}
```

**对齐两大铁律**：
1. gutter 和 body **同 font-size**（`--text-meta` = 13px）
2. gutter 和 body **同 line-height**（1.72）

这两条无论用什么 grid 还是 per-line 实现都必须满足。

### 替代方案：per-line 网格（强烈推荐用于生产）

每行一个 grid row，gutter 与 code 在同一行，天然对齐：

```html
<div class="code-v3 code-v3--grid">
  <div class="code-v3__row">
    <span class="code-v3__num">138</span>
    <span class="code-v3__src"><span class="tok-com">// agent-loop.cpp</span></span>
  </div>
  <div class="code-v3__row">
    <span class="code-v3__num">139</span>
    <span class="code-v3__src"><span class="tok-kw">while</span> (…) {</span>
  </div>
</div>
```

```css
.code-v3--grid {
  display: block;
  padding: var(--space-4) 0;
  overflow-x: auto;
}
.code-v3__row {
  display: grid;
  grid-template-columns: 44px 1fr;
  align-items: baseline;
  padding: 0 var(--space-3) 0 0;
}
.code-v3__row:hover { background: color-mix(in oklch, var(--surface-raised) 50%, transparent); }
.code-v3__num {
  text-align: right;
  color: var(--content-soft);
  user-select: none;
  padding-right: var(--space-3);
  font-size: var(--text-meta);
  line-height: 1.72;
}
.code-v3__src {
  white-space: pre;
  font-size: var(--text-meta);
  line-height: 1.72;
}
```

**为什么推荐 grid**：
- 天然行对齐
- hover 高亮整行（单栏实现做不到）
- 未来可加"行号点击 → 引用 / 跳转 / 评论"等交互
- 合并代码高亮 token 时不需要考虑跨行 span

---

## 3 · Syntax Highlighting · token palette

**引擎选择**：
- **Shiki** —— 推荐。基于 TextMate grammars，高亮保真度最高。我们的 oklch theme 可通过 `shiki` 的 theme API 注入。
- **Highlight.js** —— 次选。更轻量，但 token 粒度粗。

### Token 类 CSS（与引擎无关，引擎负责给对应 span 加这些 class）

```css
.tok-kw   { color: var(--signal-deep); font-weight: 500; }   /* 关键字：while / if / return / import / class */
.tok-str  { color: var(--accent-deep); }                      /* 字符串字面量 */
.tok-num  { color: var(--accent-deep); }                      /* 数字字面量 */
.tok-fn   { color: var(--content); font-weight: 500; }        /* 函数名（定义 + 调用） */
.tok-com  { color: var(--content-soft); font-style: italic; } /* 注释 */
.tok-type { color: var(--signal-deep); }                       /* 类型名：int / float / StopReason */
.tok-var  { color: var(--content); }                           /* 普通变量 */
.tok-op   { color: var(--content-muted); }                     /* 运算符 · 括号 */
.tok-attr { color: var(--signal-deep); }                       /* 属性 / 装饰器 */
.tok-meta { color: var(--content-soft); font-style: italic; }  /* 模板 / jsx / 特殊字面量 */
```

**共用 6 色**（不扩充到 10+ 色）：
- `--content` — 普通标识符
- `--content-muted` — 运算符
- `--content-soft` — 注释
- `--signal-deep` — 关键字 / 类型 / 装饰
- `--accent-deep` — 字符串 / 数字

**禁止**：
- ❌ 引入第 4 / 第 5 种信号色做"class 名 vs method 名"区分
- ❌ 背景高亮（除了选中 / diff 行）
- ❌ 斜体除了注释和 template literal

### Shiki theme mapping（片段）

```js
import { createHighlighter } from 'shiki';

const orangutanTheme = {
  name: 'orangutan-manuscript',
  type: 'light',
  fg: 'var(--content)',
  bg: 'var(--surface)',
  colors: { 'editor.foreground': 'var(--content)', 'editor.background': 'var(--surface)' },
  tokenColors: [
    { scope: ['comment'],                    settings: { foreground: 'var(--content-soft)', fontStyle: 'italic' } },
    { scope: ['keyword', 'storage.type'],    settings: { foreground: 'var(--signal-deep)', fontStyle: 'bold' } },
    { scope: ['entity.name.type'],           settings: { foreground: 'var(--signal-deep)' } },
    { scope: ['string', 'constant.numeric'], settings: { foreground: 'var(--accent-deep)' } },
    { scope: ['entity.name.function'],       settings: { foreground: 'var(--content)', fontStyle: 'bold' } },
    { scope: ['punctuation', 'keyword.operator'], settings: { foreground: 'var(--content-muted)' } },
  ],
};
```

**dark theme** 对应镜像一份，使用 `[data-theme="dark"]` tokens。

---

## 4 · 语言识别与 chip 文字

| 输入 | chip 文字 | name 字段 |
|---|---|---|
| ` ```cpp\nfilename:src/… ` | `cpp` | `src/…` |
| ` ```python ` 无 filename | `python` | 空 · 仅 chip |
| ` ```js ` | `js` | 空 |
| ` ``` ` 无语言 | `text` | 空 |
| ` ```sh ` / ` ```bash ` / ` ```shell ` | `shell` | 空 |
| ` ```diff ` | —— | 交给 diff 块（见 § 5） |

**统一 chip 缩写**：`javascript` → `js`、`typescript` → `ts`、`python` → `python`（不 `py`）、`c++` → `cpp`、`c#` → `csharp`、`shell` / `sh` / `bash` → `shell`。

---

## 5 · Block Diff · Unified / Split 双视图

### HTML 结构

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="block__chip">diff</span>
      <span class="block__name">agent-loop.cpp</span>
      <span class="block__meta">1 hunk <em>·</em> <span class="ok">+3</span> <em>·</em> <span class="err">−3</span></span>
    </div>
    <div class="diff-v3__toggle" role="tablist" aria-label="diff 视图">
      <button type="button" aria-pressed="true"  data-view="unified">unified</button>
      <button type="button" aria-pressed="false" data-view="split">split</button>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="复制 patch"><!-- copy --></button>
      <button class="block__btn" aria-label="在编辑器打开"><!-- external-link --></button>
    </div>
  </div>
  <div class="diff-v3" data-view="unified">
    <div class="diff-v3__hunk">@@ src/agent/agent-loop.cpp · L138–L141 @@</div>
    <div class="diff-v3__unified"><!-- unified rows --></div>
    <div class="diff-v3__split"><!-- split rows · 默认隐藏 --></div>
  </div>
</div>
```

### CSS

```css
.diff-v3 {
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  line-height: 1.74;
  overflow-x: auto;
}
.diff-v3__hunk {
  padding: 6px var(--space-4);
  background: var(--surface-raised);
  color: var(--content-soft);
  border-bottom: 1px solid var(--rule-soft);
  font-size: 11px;
  letter-spacing: .02em;
}

/* View switch · 隐藏另一视图 */
.diff-v3[data-view="unified"] .diff-v3__split   { display: none; }
.diff-v3[data-view="split"]   .diff-v3__unified { display: none; }

/* Unified rows */
.diff-v3__unified .diff__line {
  display: grid;
  grid-template-columns: 48px 1fr;
  padding: 0 var(--space-4);
  tab-size: 2;
}

/* Split rows · 两列并排 */
.diff-v3__split {
  display: grid;
  grid-template-columns: 1fr 1px 1fr;
}
.diff-v3__split .col  { min-width: 0; overflow-x: auto; }
.diff-v3__split .col__head {
  padding: 6px var(--space-4);
  background: var(--surface-raised);
  color: var(--content-soft);
  font-size: 11px;
  letter-spacing: .08em;
  text-transform: uppercase;
  border-bottom: 1px solid var(--rule-soft);
}
.diff-v3__split .col--left  .col__head { color: var(--signal-deep); }  /* before */
.diff-v3__split .col--right .col__head { color: var(--accent-deep); }  /* after */
.diff-v3__split .col__sep { background: var(--rule-soft); }
.diff-v3__split .diff__line {
  display: grid;
  grid-template-columns: 44px 1fr;
  padding: 0 var(--space-4);
  tab-size: 2;
}

/* 行类型 · 三重冗余（行号 prefix + 背景色 + 文字色） */
.diff__gutter {
  color: var(--content-soft);
  text-align: right;
  padding-right: var(--space-3);
  user-select: none;
}
.diff__line--ctx .diff__gutter::before { content: '  '; }
.diff__line--add {
  background: color-mix(in oklch, var(--accent-tint) 50%, transparent);
}
.diff__line--add .diff__gutter::before { content: '+ '; color: var(--accent-deep); }
.diff__line--del {
  background: color-mix(in oklch, var(--signal-tint) 50%, transparent);
}
.diff__line--del .diff__gutter::before { content: '- '; color: var(--signal-deep); }

/* Split 时的"占位行"· 用虚淡灰背景标示另一侧此行被删 / 新增 */
.diff__line--empty {
  background: color-mix(in oklch, var(--surface-raised) 50%, transparent);
  color: var(--content-soft);
}
.diff__line--empty .diff__gutter::before { content: '  '; }
```

### 色盲可读三重冗余

| 行类型 | gutter prefix | 背景 | 文字 |
|---|---|---|---|
| 添加 | `+ ` (Moss) | `--accent-tint` 50% | `--content` |
| 删除 | `- ` (Sienna) | `--signal-tint` 50% | `--content` |
| 上下文 | 无 prefix | `--surface` | `--content` |
| 空（split 对齐） | 无 prefix | `--surface-raised` 50% | `--content-soft` |

**三维都不可省**：色盲用户可能分不清 Moss/Sienna 背景，但能看到 `+`/`-` 前缀；色盲 + 老旧屏幕可能前缀看不清，但行号列不同。

---

## 6 · Diff · 行号策略

- **Unified view**：行号显示**后文件**的最终行号；删除行的行号用该删除行在**前文件**中的位置（可选 `title` attribute 显示另一侧的行号）。
- **Split view**：左列行号是 before 的，右列行号是 after 的。Empty 行无行号。

```html
<!-- Unified: 删除行用 before 的行号，添加行用 after 的行号 -->
<div class="diff__line diff__line--del"><span class="diff__gutter">138</span><span>while (iter &lt; MAX_ITERATIONS) {</span></div>
<div class="diff__line diff__line--add"><span class="diff__gutter">138</span><span>auto memory = …;</span></div>
```

---

## 7 · Diff · 视图偏好持久化

```js
function initDiffToggle(toggleEl) {
  const saved = localStorage.getItem('orangutan.diff.view') || 'unified';
  applyView(toggleEl, saved);

  toggleEl.querySelectorAll('button').forEach(btn => {
    btn.addEventListener('click', () => {
      const view = btn.dataset.view;
      applyView(toggleEl, view);
      localStorage.setItem('orangutan.diff.view', view);
    });
  });
}

function applyView(toggleEl, view) {
  const diff = toggleEl.closest('.block')?.querySelector('.diff-v3');
  if (!diff) return;
  diff.dataset.view = view;
  toggleEl.querySelectorAll('button').forEach(b => {
    b.setAttribute('aria-pressed', b.dataset.view === view ? 'true' : 'false');
  });
}
```

**全局一致**：所有 diff 块共享同一个视图偏好。用户在一处切到 split，全屏其余 diff 也变 split。

---

## 8 · 长 diff 折叠 · 200 行阈值

```css
.diff-v3 {
  max-height: 600px;
  overflow-y: auto;
}
.diff-v3[data-folded="true"] {
  max-height: 360px;
  overflow-y: hidden;
  position: relative;
}
.diff-v3[data-folded="true"]::after {
  content: ''; position: absolute; inset: auto 0 0 0; height: 48px;
  background: linear-gradient(transparent, var(--surface));
  pointer-events: none;
}
```

阈值：总行数 > 200 时 `data-folded="true"`，bar 右侧出现额外的"显示完整"按钮（Phosphor `ArrowsOutLineVertical` / Lucide `expand`）。

---

## 9 · 操作按钮 · 行为规格

| 按钮 | 行为 |
|---|---|
| `aria-label="复制代码"` | 将 `.code-v3__body` 纯文本（剥离 tag）拷入 clipboard · 视觉反馈：按钮 800ms 内显示一个 `✓` small SVG 替代 icon |
| `aria-label="在编辑器打开"` | 打开 `vscode://file/{absolutePath}:{startLine}` —— 仅在 `.block__name` 是绝对路径时生效（相对路径则 `href` disabled） |
| `aria-label="运行"` | 仅对 `python` / `shell` 语言生效；点击后发 POST `/api/v1/tools/execute` · 将 stdout 作为新的 tool-output 块插入 |
| `aria-label="复制 patch"`（diff） | 拷贝 unified diff 格式的 patch 字符串，带 `---` / `+++` 头 |
| `aria-label="折叠"` | diff / code 内部长内容折叠切换（与 tool/thinking 的 is-open 无关） |

**复制反馈 CSS**：

```css
.block__btn[data-state="copied"] {
  color: var(--accent-deep);
  pointer-events: none;
}
.block__btn[data-state="copied"] svg {
  /* 隐藏原 icon */ visibility: hidden;
}
.block__btn[data-state="copied"]::after {
  content: '✓';
  font-family: var(--font-mono);
  font-size: 14px;
  color: var(--accent-deep);
  position: absolute;
}
```

---

## 10 · Inline Diff · 行内三重冗余

触发条件：agent 正文里用 `<del>old</del><ins>new</ins>` 或 agent 侧 markdown pipeline 识别"把 X 改成 Y"语义。

```html
<p class="idiff">
  顺手把 <del>MAX_ITERATIONS=20</del><ins>MAX_ITERATIONS=25</ins> 也提上来。
</p>
```

### CSS

```css
.idiff { font-family: var(--font-body); line-height: 1.72; }
.idiff ins {
  background: color-mix(in oklch, var(--accent-tint) 80%, transparent);
  text-decoration: none;
  color: var(--accent-deep);
  padding: 1px 5px;
  border-radius: var(--radius-sm);
  font-family: var(--font-mono);
  font-size: .9em;
}
.idiff del {
  background: color-mix(in oklch, var(--signal-tint) 80%, transparent);
  text-decoration: line-through;
  text-decoration-color: var(--signal);
  color: var(--signal-deep);
  padding: 1px 5px;
  border-radius: var(--radius-sm);
  font-family: var(--font-mono);
  font-size: .9em;
  margin-right: 3px;
}
```

**三重冗余**：
- 背景色（Moss/Sienna tint）
- 文字色（accent-deep / signal-deep）
- 额外标记：`ins` 无额外记号；`del` 有 `text-decoration: line-through`

**相邻规则**：`del` + `ins` 相邻时中间不加 "→" 箭头，仅靠视觉对比传达变化。

---

## 11 · 代码中的特殊字符

| 字符 | 处理 |
|---|---|
| `<` `>` `&` | HTML escape 为 `&lt;` `&gt;` `&amp;`（parser 做） |
| Tab | `tab-size: 2` 渲染为 2 空格宽度（`letter-spacing: 0` 避免漂移） |
| Unicode 字符 | 保留原样（Spline Sans Mono 支持拉丁、数字、常用符号；中文字符走 fallback chain 的 `ui-monospace`） |
| 不可见字符（BOM、零宽） | **渲染前剥离**；防止 agent 输出 BOM 让 copy 出错 |
| Windows CRLF | `\r\n` → `\n`（渲染前规范化） |

---

## 12 · 自检

### Code
- [ ] gutter 与 body 同 `font-size` / 同 `line-height`
- [ ] chip 中性（不 Sienna / 不 Moss）
- [ ] 复制按钮有 `aria-label`
- [ ] token 色板只有 5 种（content · content-muted · content-soft · signal-deep · accent-deep），加 comment italic
- [ ] 暗色主题下代码背景是 `--surface`（非浓黑）
- [ ] 长代码 > 80 行时支持折叠

### Diff
- [ ] Unified / Split 均实现，toggle 持久化到 localStorage
- [ ] 行号 gutter 前缀 `+ ` / `- `，色盲用户能识别
- [ ] 背景 + 文字色 + 前缀三重冗余
- [ ] Moss = add，Sienna = del（不反过来）
- [ ] 长 diff > 200 行支持折叠到 360px，带 fade 遮罩

### Inline Diff
- [ ] `del` 有 `line-through` + signal 色 + tint 背景三重冗余
- [ ] `ins` 有 accent 色 + tint 背景（无 `line-through`）
- [ ] 相邻 `del` + `ins` 不插 "→"
