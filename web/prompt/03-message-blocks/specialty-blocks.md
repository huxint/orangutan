# Specialty Blocks · Tool · Thinking · Reply-Quote · Image

> 聊天流里的四类"辅助"块：工具调用、思考链、回复引用、图像。
> Tool 与 Thinking **默认折叠**（调用证据 / 辅助思考，不打扰正文）；Reply-Quote 与 Image 按内容性质自然呈现。

---

## 1 · Tool Block · 工具调用

### 1.1 · HTML 结构

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--ok" aria-hidden="true"></span>
      <span class="block__chip">tool</span>
      <span class="block__name">read_file</span>
      <span class="block__meta">
        src/agent/agent-loop.cpp
        <em>·</em> <span class="ok">ok</span>
        <em>·</em> 142 ms
      </span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" data-action="toggle-tool" aria-label="展开 / 折叠" aria-expanded="false">
        <svg class="icon"><!-- chevron-down --></svg>
      </button>
      <button class="block__btn" aria-label="复制工具输出"><!-- copy icon --></button>
      <button class="block__btn" aria-label="在编辑器打开"><!-- external-link --></button>
    </div>
  </div>
  <!-- body 默认不可见 · 无 is-open 时 display:none -->
  <div class="tool-v3" role="region" aria-labelledby="…">
<span class="arrow">→</span> read 312 行
<span class="arrow">→</span> MAX_ITERATIONS = 25 · …
  </div>
</div>
```

### 1.2 · bar 左侧元信息布局

| 位 | 内容 | 必须 / 可选 |
|---|---|---|
| `.dot` | 状态：`--run` 执行中（pulse Moss）· `--ok` 成功（静 Moss）· `--err` 失败（静 Sienna） | 必须 |
| `.block__chip` | 永远显示 `tool` 字样 | 必须 |
| `.block__name` | 工具名：`read_file` / `edit_file` / `shell` / `search` / `spawn_agent`… | 必须 |
| `.block__meta` | 首要参数（文件路径 / 命令 / 关键参数）· 状态文字（`ok` / `err`）· 耗时（`142 ms`） | 可选 · 但至少一个 |

**meta 组合公式**：`{首要参数} · {状态} · {duration}`；用 `<em>·</em>` 做 separator（italic dot）。

### 1.3 · body 样式

```css
.tool-v3 {
  padding: var(--space-4) var(--space-5);
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  line-height: 1.7;
  color: var(--content);
  white-space: pre-wrap;
  display: none;            /* 默认折叠 */
}
.tool-v3.is-open { display: block; }
.tool-v3 .arrow { color: var(--content-muted); }
```

**默认折叠铁律**：`display: none` 而**非** `max-height: 0`。原因见 `block-anatomy.md § 7`。

### 1.4 · 连续调用合并

Agent 连续读 3 个文件 → 视觉上聚合为**一个 tool block**，展开后才看到逐条：

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--ok"></span>
      <span class="block__chip">tool</span>
      <span class="block__name">read_file × 3</span>
      <span class="block__meta">
        agent-loop.cpp · provider.hpp · memory.cpp
        <em>·</em> <span class="ok">all ok</span>
        <em>·</em> 417 ms
      </span>
    </div>
    <!-- toggle + copy -->
  </div>
  <div class="tool-v3">
    <!-- 每条独立的子块（仍是 .tool-v3 sub-section，无 bar） -->
    <section class="tool-v3__sub">
      <header><span class="arrow">→</span> read_file · src/agent/agent-loop.cpp · 142 ms · ok</header>
      <pre>…</pre>
    </section>
    <section class="tool-v3__sub">…</section>
    <section class="tool-v3__sub">…</section>
  </div>
</div>
```

**合并规则**：
- 同一个 tool name
- 同一 agent（lead 或特定 teammate）
- 在对话流中**连续**（中间无其他块切分）
- 最多合并 **10 条**；超过分两组

### 1.5 · 结构化输出探测

Tool 的 body 可以是纯文本，也可能是结构化：

| 输出类型 | 探测依据 | 渲染 |
|---|---|---|
| JSON object / array | `{`/`[` 起、合法 JSON parse | JSON tree（折叠式） |
| diff / patch | 行首 `@@` / `+` / `-` pattern | 嵌套渲染为 `.diff-v3`（见 `code-and-diff.md`） |
| file list（ls 输出） | 每行 `{mode} {user} {group} {size} {date} {name}` | 保留 mono · 不特殊处理 |
| 纯文本 | 其他一切 | mono pre-wrap |

**JSON 树示例**：

```html
<div class="tool-v3 is-open">
  <div class="json-tree">
    <details open>
      <summary>{ 3 keys }</summary>
      <div class="json-row"><span class="json-key">"status"</span>: <span class="json-str">"ok"</span></div>
      <div class="json-row"><span class="json-key">"count"</span>: <span class="json-num">312</span></div>
      <details>
        <summary><span class="json-key">"files"</span>: [ 3 ]</summary>
        <div class="json-row"><span class="json-str">"agent-loop.cpp"</span></div>
        <div class="json-row"><span class="json-str">"provider.hpp"</span></div>
      </details>
    </details>
  </div>
</div>
```

```css
.json-tree { font-family: var(--font-mono); font-size: var(--text-meta); line-height: 1.7; }
.json-tree summary { cursor: pointer; color: var(--content); padding: 2px 0; }
.json-tree details { padding-left: var(--space-4); }
.json-tree details[open] > summary { color: var(--content-muted); }
.json-tree .json-key { color: var(--signal-deep); }
.json-tree .json-str { color: var(--accent-deep); }
.json-tree .json-num { color: var(--accent-deep); }
.json-tree .json-bool { color: var(--signal-deep); font-weight: 500; }
.json-tree .json-null { color: var(--content-soft); font-style: italic; }
```

### 1.6 · 错误态 · 自动展开

Tool 失败时：
- `.dot--err` 取代 `.dot--ok`
- `.block__meta` 的状态文字用 `<span class="err">err</span>`
- `.tool-v3` **自动加 `.is-open`**（无需用户点击即可看到错误）
- 若可重试（如网络错误），bar 右侧显示 `Refresh` icon button

### 1.7 · 权限待审批态

当 tool 需要权限审批（permissions: ask）时，block 自成一种 pending 态：

```html
<div class="block block--pending">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--run"></span>
      <span class="block__chip">tool · 待审批</span>
      <span class="block__name">shell</span>
      <span class="block__meta">rm -rf build/ · 需要用户批准</span>
    </div>
    <div class="block__rhs">
      <button class="btn btn--signal btn--sm">批准调用</button>
      <button class="btn btn--ghost btn--sm">驳回</button>
    </div>
  </div>
  <!-- 无 body -->
</div>
```

（`.btn` 来自 Step 01）

---

## 2 · Thinking Block · 思考链

### 2.1 · HTML 结构

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <!-- 流式中 run · 完成后 ok -->
      <span class="dot dot--run" aria-hidden="true"></span>
      <span class="block__name">Thinking</span>
      <span class="block__meta">4 s <em>·</em> 218 tokens <em>·</em> streaming…</span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" data-action="toggle-think" aria-label="展开思考" aria-expanded="false">
        <svg class="icon"><!-- chevron-down --></svg>
      </button>
    </div>
  </div>
  <div class="think-v3" role="region">
    <!-- 思考原文 · 只在展开时显示 -->
    <p>用户同时给了两个任务…</p>
  </div>
</div>
```

### 2.2 · CSS

```css
.think-v3 {
  color: var(--content-muted);
  font-family: var(--font-body);
  font-style: italic;
  font-size: var(--text-ui);
  line-height: 1.68;
  padding: var(--space-3) var(--space-5);
  display: none;
}
.think-v3.is-open { display: block; }
.think-v3 p { margin: var(--space-2) 0; max-width: 60ch; }
.think-v3 p:first-child { margin-top: 0; }
.think-v3 p:last-child  { margin-bottom: 0; }
.think-v3 code {
  font-family: var(--font-mono);
  font-size: .88em;
  background: var(--surface-raised);
  padding: 1px 5px;
  border-radius: var(--radius-sm);
  color: var(--content-muted);
  font-style: normal;
}
```

### 2.3 · 仅 lead 显示 · Teammate thinking 走右栏

**主聊天流**（middle-chat）中：
- **lead 的 thinking block 正常显示**（默认折叠 + 用户可点开）
- **Teammate 的 thinking 不显示**在主流
- 在左栏点击 teammate card 打开右栏后，**右栏时间线完整显示 teammate 的 thinking + tool + 内部回复**（见 Step E · right-panel 细化）

**渲染策略**：
- 主聊天流的消息流处理器过滤 `author.role === 'teammate' && event.type === 'thinking'` 的事件
- 这些事件仍然被缓存在 EventBus（Step 02 backend redesign），right-panel 订阅时取全量

### 2.4 · 流式态 vs 完成态

| 阶段 | bar 左侧 | meta 文案 | 行为 |
|---|---|---|---|
| 流式中 | `.dot--run`（pulse） | `{已 s} s · {tokens} tokens · streaming…` | body 不自动展开；bar 每秒刷新 meta；用户可随时展开看流式内容 |
| 完成 | `.dot--ok`（静） | `{总 s} s · {总 tokens} tokens · claude-opus-4-7` | meta 变为 model 名 + 总计；body 不主动收回 |

**避免**：
- ❌ 完成时自动"收折"（如果用户已展开在看，会被打断）
- ❌ 完成时自动"弹出"（打扰正文）
- ✅ 保持用户的展开状态不变

---

## 3 · Reply-Quote · 手稿注脚 + 放大引号

### 3.1 · HTML 结构

```html
<div class="rq-v3">
  <div class="rq-v3__head">
    <span class="rq-v3__meta">Re · lead · 22:14</span>
    <span class="rq-v3__rule"></span>
  </div>
  <div class="rq-v3__body" role="link" tabindex="0"
       data-ref="msg-abc123" aria-label="跳回原消息 · lead · 22:14">
    <span class="rq-v3__mark" aria-hidden="true">&ldquo;</span>
    <span class="rq-v3__text">
      查一下 Anthropic 文档里 prompt-caching 的 TTL 上限——我怀疑 ephemeral 的 5 分钟上限在实战里会导致很多缓存 miss。
    </span>
  </div>
</div>
```

### 3.2 · CSS

```css
.rq-v3 { margin: 0 0 var(--space-5) 0; }

.rq-v3__head {
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: var(--space-3);
  margin-bottom: var(--space-3);
}
.rq-v3__meta {
  font-family: var(--font-mono);
  font-size: 11px;
  letter-spacing: .12em;
  text-transform: uppercase;
  color: var(--content-soft);
  white-space: nowrap;
}
.rq-v3__rule {
  height: 1px;
  background: var(--rule-soft);
}

.rq-v3__body {
  display: grid;
  grid-template-columns: 44px 1fr;
  gap: var(--space-3);
  align-items: start;
  padding: 0 var(--space-2);
  cursor: pointer;
  transition: background var(--duration-fast) var(--ease);
  border-radius: var(--radius-sm);
}
.rq-v3__body:hover {
  background: color-mix(in oklch, var(--surface-raised) 50%, transparent);
}
.rq-v3__body:focus-visible {
  outline: 2px solid var(--signal);
  outline-offset: 2px;
  background: color-mix(in oklch, var(--surface-raised) 50%, transparent);
}

.rq-v3__mark {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 48px;
  line-height: .82;
  color: var(--content-soft);
  text-align: center;
  user-select: none;
  letter-spacing: -.02em;
}

.rq-v3__text {
  font-family: var(--font-display);
  font-style: italic;
  font-size: 16px;
  line-height: 1.58;
  color: var(--content-muted);
  max-width: 60ch;
  padding: 10px 0 4px;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}
```

### 3.3 · 引号字符

- 使用 **`"` (U+201C · LEFT DOUBLE QUOTATION MARK)** 作为开引号
- **不使用** `❝` (U+275D)、`„` (U+201E)、`「`(U+300C)
- 仅放一个**开引号**（左侧），不放闭引号——视觉上由 `.rq-v3__head` 的 rule 线"闭合"这一块

### 3.4 · 跳回原文交互

点击 `.rq-v3__body` 或 Enter / Space 触发跳回：

```js
document.addEventListener('click', (e) => {
  const body = e.target.closest('.rq-v3__body[data-ref]');
  if (!body) return;
  scrollToMessage(body.dataset.ref);
});

document.addEventListener('keydown', (e) => {
  if (e.key !== 'Enter' && e.key !== ' ') return;
  const body = e.target.closest('.rq-v3__body[data-ref]');
  if (!body) return;
  e.preventDefault();
  scrollToMessage(body.dataset.ref);
});

function scrollToMessage(refId) {
  const target = document.querySelector(`[data-msg-id="${CSS.escape(refId)}"]`);
  if (!target) return;

  const scroller = document.querySelector('.chat-scroll');
  if (!scroller) return;

  // 禁用 scrollIntoView · 手动计算 offsetTop
  const scrollerRect = scroller.getBoundingClientRect();
  const targetRect = target.getBoundingClientRect();
  const delta = targetRect.top - scrollerRect.top;
  scroller.scrollTop += delta - 24;   // 留 24px 顶部呼吸

  // 高亮淡出效果
  target.classList.add('msg--highlight');
  setTimeout(() => target.classList.remove('msg--highlight'), 800);
}
```

### 3.5 · 高亮目标样式

```css
.msg--highlight {
  animation: msg-highlight 800ms var(--ease) 0s 1;
}
@keyframes msg-highlight {
  0%   { background: color-mix(in oklch, var(--signal-tint) 45%, transparent); }
  100% { background: transparent; }
}

@media (prefers-reduced-motion: reduce) {
  .msg--highlight {
    animation: none;
    background: color-mix(in oklch, var(--signal-tint) 30%, transparent);
    transition: background 400ms linear;
  }
}
```

每个 `.msg` 必须带 `data-msg-id`：

```html
<article class="msg" data-msg-id="msg-abc123">…</article>
```

### 3.6 · 长引用的行为

`.rq-v3__text` 默认 2 行 ellipsis。用户**点击**已经触发跳回原消息——因此**不提供"展开引用"**选项（否则行为歧义）。原消息才是 truth of the quote；引用只是 peek。

---

## 4 · Image Block · 图像块

### 4.1 · HTML 结构 · 三态

**state: loading（生成中）**

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--run"></span>
      <span class="block__chip">image</span>
      <span class="block__name">hit-rate-curve.svg</span>
      <span class="block__meta">480 × 220 <em>·</em> generating…</span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="取消" disabled style="opacity:.38">
        <svg class="icon"><!-- x icon --></svg>
      </button>
    </div>
  </div>
  <div class="image-v3">
    <div class="image-v3__frame">image · 480 × 220</div>
    <div class="image-v3__caption">—— 预期命中率 vs 请求到达率 · 生成中</div>
  </div>
</div>
```

**state: loaded**

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--ok"></span>
      <span class="block__chip">image</span>
      <span class="block__name">hit-rate-curve.svg</span>
      <span class="block__meta">480 × 220 <em>·</em> 14 KB</span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="查看大图"><!-- eye icon --></button>
      <button class="block__btn" aria-label="下载"><!-- download icon --></button>
    </div>
  </div>
  <div class="image-v3">
    <img class="image-v3__img" src="…" alt="hit-rate 曲线图" loading="lazy" />
    <div class="image-v3__caption">—— 预期命中率 vs 请求到达率</div>
  </div>
</div>
```

**state: error**

```html
<div class="block">
  <div class="block__bar">
    <div class="block__lhs">
      <span class="dot dot--err"></span>
      <span class="block__chip">image</span>
      <span class="block__name">hit-rate-curve.svg</span>
      <span class="block__meta"><span class="err">生成失败</span></span>
    </div>
    <div class="block__rhs">
      <button class="block__btn" aria-label="重试"><!-- refresh icon --></button>
    </div>
  </div>
  <div class="image-v3">
    <div class="image-v3__frame image-v3__frame--err">
      image failed · placeholder
    </div>
    <div class="image-v3__caption image-v3__caption--err">—— 生成失败：Context deadline exceeded · 点击重试</div>
  </div>
</div>
```

### 4.2 · CSS

```css
.image-v3 { overflow: hidden; }

.image-v3__img {
  display: block;
  width: 100%;
  height: auto;
  max-height: 420px;
  object-fit: contain;
  background: var(--surface);
  border-top: 1px solid var(--rule-soft);
}

.image-v3__frame {
  height: 200px;
  display: flex;
  align-items: center;
  justify-content: center;
  background:
    repeating-linear-gradient(135deg,
      var(--surface-raised), var(--surface-raised) 6px,
      var(--surface) 6px, var(--surface) 12px);
  font-family: var(--font-mono);
  font-size: var(--text-caption);
  color: var(--content-soft);
  letter-spacing: .1em;
  text-transform: uppercase;
  border-top: 1px dashed var(--rule);
  border-bottom: 1px dashed var(--rule);
}
.image-v3__frame--err {
  background: color-mix(in oklch, var(--signal-tint) 20%, var(--surface));
  color: var(--signal-deep);
  border-color: oklch(from var(--signal) l c h / .35);
}

.image-v3__caption {
  font-family: var(--font-body);
  font-style: italic;
  font-size: var(--text-ui);
  color: var(--content-muted);
  text-align: center;
  padding: var(--space-3);
}
.image-v3__caption--err {
  color: var(--signal-deep);
  font-style: normal;
  cursor: pointer;
}
```

### 4.3 · Lightbox · 本 step 不实现

点击"查看大图"暂作桩：

```js
btn.addEventListener('click', () => {
  console.warn('[TODO] lightbox · Step I (Orangutan 资产) 或 Step G (image viewer)');
});
```

**Lightbox 规格**将来由独立 step 定义。核心原则：
- 覆盖层 overlay（`z-index` 从 `--z-modal`）
- ESC 关闭 · 焦点回到触发按钮
- 禁用 `scrollIntoView`

---

## 5 · 速查

| 块 | 默认展开 | bar 左侧身份 | 失败时自动展开？ |
|---|---|---|---|
| Tool | collapsed | dot + chip + name + meta | ✅ |
| Thinking | collapsed | dot + name + meta（无 chip） | ❌（失败不可能；都是 LLM 内部） |
| Reply-Quote | n/a（无 bar） | rule + meta（head 层） | n/a |
| Image | loading / loaded / error 三态均展开 | dot + chip + name + meta | ✅ |

---

## 6 · 自检

### Tool
- [ ] bar 左侧 `dot` + `chip(tool)` + `name` + `meta`
- [ ] body 默认 `display: none`，`.is-open` 才显示
- [ ] 状态 dot 三态（run / ok / err），动画遵守 `prefers-reduced-motion`
- [ ] 连续相同工具调用合并到一个 block
- [ ] 错误态自动 `.is-open`
- [ ] 权限待审批态有批准 / 驳回按钮

### Thinking
- [ ] 仅 lead 的 thinking 出现在主聊天流
- [ ] 流式中 bar `.dot--run`（pulse），完成后 `.dot--ok`
- [ ] body `display: none` 默认
- [ ] 不自动展开 / 不自动收回

### Reply-Quote
- [ ] head = mono `Re · lead · 时间` + 薄 rule
- [ ] body = 44px `"` 大引号 + italic 文字（max 2 行 ellipsis）
- [ ] 点击跳回原文 · **不用 scrollIntoView** · 用 `scrollTop` 计算
- [ ] 跳转后 `.msg--highlight` 800ms 淡出
- [ ] body 可键盘聚焦 + Enter / Space 跳转

### Image
- [ ] 三态（loading / loaded / error）切换 · dot + meta 同步
- [ ] loaded 态 `<img>` 接管 frame
- [ ] error 态 bar `.dot--err` + caption 有"重试"cue
- [ ] 生成中 bar pulse + caption 有"生成中"文字
