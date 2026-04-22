# 组件原子 · Component Atoms

> 每个 UI 原子的结构、CSS 关键值、变体与交互态。
> 所有值必须引用 `design-tokens.md` 的令牌；**禁止在本页的 CSS 示例外重写任何色值**。

实现顺序建议：按本文档自上而下顺序实现，下层组件复用上层。

---

## 1 · Button

### 结构

```html
<button class="btn">发送</button>
<button class="btn btn--signal">批准调用</button>
<button class="btn btn--secondary">取消</button>
<button class="btn btn--ghost">跳过</button>
<button class="btn btn--danger">驳回</button>
<button class="btn" disabled>已停用</button>
```

### 通用 CSS

```css
.btn {
  font-family: var(--font-body);
  font-size: var(--text-ui);
  font-weight: 500;
  line-height: 1;
  letter-spacing: .01em;
  padding: 10px 18px;
  min-height: 40px;
  border: none;
  border-radius: var(--radius-md);
  background: var(--content);
  color: var(--surface);
  cursor: pointer;
  box-shadow: var(--shadow-1);
  transition: background var(--duration-fast) var(--ease),
              transform var(--duration-fast) var(--ease);
}
.btn:hover  { background: oklch(from var(--content) calc(l + 0.08) c h); }
.btn:active { transform: translateY(1px); }
.btn:focus-visible {
  outline: none;
  box-shadow: var(--shadow-1), 0 0 0 3px oklch(from var(--signal) l c h / .25);
}
.btn[disabled] { opacity: .42; cursor: not-allowed; box-shadow: none; }
```

### 变体

- **`.btn--signal`** — `background: var(--signal); color: var(--surface);` hover → `--signal-deep`
  *用于：* 批准调用 · 主 CTA · 发送（含附件时）
- **`.btn--secondary`** — `background: var(--surface); color: var(--content); border: 1px solid var(--content); box-shadow: none;`
  *用于：* 取消 · 次要操作
- **`.btn--ghost`** — `background: transparent; color: var(--content-muted); border: 1px solid var(--rule); box-shadow: none;`  hover → 文字变 `--content`、描边变 `--content`
  *用于：* 跳过 · 忽略 · 弱操作
- **`.btn--danger`** — `background: transparent; color: var(--signal-deep); border: 1px solid var(--signal); box-shadow: none;`
  *用于：* 驳回 · 删除 · 不可逆破坏性操作

### 尺寸变体
- 默认 40px 高（主要）
- `.btn--sm` 32px 高 / `padding: 6px 12px` / `font-size: 13px`（用于消息内嵌按钮、tool 块内操作）
- `.btn--lg` 48px 高 / `padding: 14px 24px` / `font-size: 15px`（空状态大按钮）

### 必守约束
- 点击目标永远 ≥ 40px 高度（移动端 44px，需要时加 media query）
- 不得在 hover 加放大（transform scale 禁用）
- 不得改变字体家族 —— 始终 Newsreader

---

## 2 · Badge

### 结构

```html
<span class="badge">agent:default</span>
<span class="badge badge--moss">运行中 · 3s</span>
<span class="badge badge--sienna">等待审批</span>
<span class="badge badge--ink">lead</span>
```

### CSS

```css
.badge {
  font-family: var(--font-mono);
  font-size: 11px;
  font-weight: 500;
  letter-spacing: .06em;
  padding: 3px 9px;
  border-radius: 999px;                 /* 胶囊 */
  background: var(--surface-raised);
  color: var(--content-muted);
  border: 1px solid var(--rule-soft);
  white-space: nowrap;
  display: inline-block;
  vertical-align: middle;
}
.badge--moss {
  color: var(--accent-deep);
  background: var(--accent-tint);
  border-color: transparent;
}
.badge--sienna {
  color: var(--signal-deep);
  background: var(--signal-tint);
  border-color: transparent;
}
.badge--ink {
  color: var(--surface);
  background: var(--content);
  border-color: transparent;
}
```

### 用法规则
- 默认徽章：中性状态（"claude-sonnet-4"、"permissions: ask"、模型名）
- `.badge--moss`：成功 / 运行中 / 已完成
- `.badge--sienna`：警告 / 等待审批 / 失败
- `.badge--ink`：身份强调（"lead"、"agent"、"you"）
- **一屏徽章数量 ≤ 8**，超出视为信息过载

---

## 3 · Field（input / textarea）

### 结构

```html
<div class="field">
  <label for="q">搜索会话</label>
  <input id="q" type="text" value="token + cache" />
  <span class="field__hint">⌘ K 打开 · 支持正则</span>
</div>

<div class="field">
  <label for="msg">发送新消息</label>
  <textarea id="msg" rows="3"></textarea>
  <span class="field__hint">Shift + Enter 换行 · Enter 发送</span>
</div>
```

### CSS

```css
.field {
  display: flex;
  flex-direction: column;
  gap: var(--space-2);
  max-width: 480px;                     /* 输入默认上限，overridable */
}
.field label {
  font-family: var(--font-mono);
  font-size: var(--text-caption);
  text-transform: uppercase;
  letter-spacing: .08em;
  color: var(--content-muted);
}
.field input,
.field textarea {
  font-family: var(--font-body);
  font-size: var(--text-body);
  line-height: 1.5;
  background: var(--surface);
  color: var(--content);
  border: 1px solid var(--rule);
  border-radius: var(--radius-md);
  padding: 10px 14px;
  outline: none;
  transition: border-color var(--duration-fast) var(--ease),
              box-shadow var(--duration-fast) var(--ease);
}
.field textarea { resize: vertical; min-height: 88px; line-height: 1.6; }
.field input:focus,
.field textarea:focus {
  border-color: var(--content);
  box-shadow: 0 0 0 3px oklch(from var(--signal) l c h / .15);
}
.field__hint {
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--content-soft);
}
.field--error input,
.field--error textarea {
  border-color: var(--signal);
  box-shadow: 0 0 0 3px oklch(from var(--signal) l c h / .18);
}
.field--error .field__hint { color: var(--signal-deep); }
```

### 禁止
- 不加左侧强调色竖条（V2 必须是完整四边边框，这是反 AI-slop 核心规则之一）
- 禁用状态不用斜体 placeholder；直接降低 `--content-soft` 的可见度

---

## 4 · Avatar

### 结构

```html
<span class="avatar avatar--user">H</span>
<span class="avatar avatar--lead"><em>L</em></span>
<span class="avatar avatar--team">R</span>
```

### CSS

```css
.avatar {
  width: 52px;
  height: 52px;
  border-radius: 50%;
  background: var(--surface-sunken);
  color: var(--content);
  display: inline-flex;
  align-items: center;
  justify-content: center;
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 24px;
  letter-spacing: 0;
  box-shadow: inset 0 0 0 1px oklch(22% 0.035 255 / .12);
  flex-shrink: 0;
}
.avatar--lead {
  background: var(--signal-tint);
  color: var(--signal-deep);
  box-shadow: inset 0 0 0 1px oklch(55% 0.13 42 / .35);
}
.avatar--team {
  background: var(--accent-tint);
  color: var(--accent-deep);
  box-shadow: inset 0 0 0 1px oklch(44% 0.08 150 / .4);
}
.avatar--small  { width: 36px; height: 36px; font-size: 18px; }
.avatar--large  { width: 64px; height: 64px; font-size: 28px; }
```

### 规则
- 用户（user）avatar 用默认 surface-sunken
- 主 agent（lead）永远 Sienna-tint
- Teammate 永远 Moss-tint（副色）—— 若有多个 teammate，**每个 teammate 分配不同 hue**（会在 Step D 侧栏组件确定具体方案），但永远不是 Sienna
- 内嵌字母 `<em>L</em>` 让 Newsreader 斜体成为身份视觉签名

---

## 5 · Message 容器

### 结构

```html
<div class="msg msg--user">
  <span class="avatar">H</span>
  <div class="msg__col">
    <div class="msg__head">
      <span class="msg__name">Huxint</span>
      <span class="msg__dot">·</span>
      <span class="msg__time">22:14</span>
    </div>
    <div class="msg__body"><!-- 内容块 --></div>
  </div>
</div>
```

### CSS

```css
.msg {
  display: grid;
  grid-template-columns: 52px 1fr;
  gap: var(--space-4);
  padding: var(--space-4) 0;
}
.msg + .msg { border-top: 1px dashed var(--rule-soft); }

.msg__head {
  display: flex;
  align-items: baseline;
  gap: var(--space-3);
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  color: var(--content-muted);
  letter-spacing: .02em;
  margin-bottom: var(--space-2);
  flex-wrap: wrap;
}
.msg__name {
  color: var(--content);
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 16px;
  letter-spacing: 0;
}
.msg__role {
  color: var(--signal-deep);
  font-family: var(--font-display);
  font-style: italic;
  font-size: 14px;
  font-weight: 400;
}
.msg__dot { color: var(--rule); }

.msg__body {
  font-family: var(--font-body);
  font-size: var(--text-body);
  line-height: 1.64;
  color: var(--content);
}
.msg__body p { margin: var(--space-2) 0; }
.msg__body p:first-child { margin-top: 0; }
.msg__body p:last-child  { margin-bottom: 0; }
```

### 变体
- `.msg--user` — 默认
- `.msg--lead` — agent avatar 用 `.avatar--lead`，msg__role 显示"主协调"
- `.msg--team` — teammate 回复，msg__role 显示"委派"

### 规则
- 消息之间**只用虚线 `border-top: 1px dashed var(--rule-soft)` 分隔**——不用气泡框，不用背景色块
- 每条消息内部块（thinking / tool / diff / image）之间间距为 `--space-3`
- 消息不限宽，但 `.msg__body` 内的段落应用 `max-width: 64ch; text-wrap: pretty;`

---

## 6 · Thinking Block

### 结构

```html
<div class="think">
  <span class="think__marker">Thinking · 4s</span>
  <span class="think__text">
    用户同时发出两个请求：本地代码 audit 我自己来，文档查询委派给 research。
  </span>
</div>
```

### CSS

```css
.think {
  margin: var(--space-3) 0;
  padding: var(--space-4) var(--space-5);
  background: color-mix(in oklch, var(--surface-raised) 60%, transparent);
  border-radius: var(--radius-md);
  color: var(--content-muted);
  font-family: var(--font-body);
  font-style: italic;
  font-size: var(--text-ui);
  line-height: 1.68;
  display: grid;
  grid-template-columns: auto 1fr;
  gap: var(--space-4);
}
.think__marker {
  font-family: var(--font-mono);
  font-style: normal;
  font-size: 11px;
  letter-spacing: .12em;
  text-transform: uppercase;
  color: var(--content-soft);
  align-self: start;
  padding-top: 4px;
  padding-right: var(--space-4);
  border-right: 1px solid var(--rule);
  white-space: nowrap;
}
```

### 规则
- 仅 lead agent 才显示 thinking 块；teammate 的 thinking 不显示在主聊天区（在右侧详情区显示——Step E）
- 默认**折叠后只显示第一行 + "展开"**；Step C 细化折叠逻辑
- 右侧边的 `border-right: 1px solid var(--rule)` 是 V2 允许的例外——因为它在内部装饰，不是"左竖条容器"反 pattern

---

## 7 · Tool Block

### 结构

```html
<div class="tool">
  <div class="tool__head">
    <span class="tool__lhs">
      <span class="tool__glyph"></span>
      <span class="tool__name">read_file</span>
      <span class="tool__arg">src/agent/agent-loop.hpp</span>
    </span>
    <span class="tool__status tool__status--ok">ok · 142 ms</span>
  </div>
  <div class="tool__body">
    → 共 312 行
    → 发现 MAX_ITERATIONS=25
  </div>
</div>
```

### CSS

```css
.tool {
  margin: var(--space-3) 0;
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  background: var(--surface);
  box-shadow: var(--shadow-1);
  overflow: hidden;
  font-family: var(--font-mono);
  font-size: var(--text-meta);
}
.tool__head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 10px var(--space-4);
  background: var(--surface-raised);
  color: var(--content-muted);
  border-bottom: 1px solid var(--rule-soft);
}
.tool__lhs { display: flex; align-items: center; gap: var(--space-3); }
.tool__glyph {
  width: 18px; height: 18px;
  border: 1.5px solid var(--content);
  border-radius: var(--radius-sm);
  position: relative;
  flex-shrink: 0;
}
.tool__glyph::after {
  content: ''; position: absolute; inset: 3px;
  background: var(--signal);
  border-radius: 1px;
}
.tool__name { color: var(--content); font-weight: 600; }
.tool__arg  { color: var(--content-muted); }
.tool__status { font-size: 11px; letter-spacing: .08em; }
.tool__status--ok  { color: var(--accent-deep); }
.tool__status--err { color: var(--signal-deep); }
.tool__body {
  padding: var(--space-4);
  white-space: pre-wrap;
  color: var(--content);
  line-height: 1.62;
  max-height: 220px;
  overflow: auto;
}
```

### 交互
- 默认折叠到 `max-height: 220px`，超出点击展开
- 折叠状态底部加线性 fade（`linear-gradient` 从 transparent 到 `--surface`，仅 48px 高度，这是唯一允许的 fade 渐变）
- 展开后 max-height: 80vh

### 规则
- `tool__glyph` 内嵌小方块用 `--signal` — 代表"工具即将执行"的赭红笔触感
- 多工具连续调用时，**合并成数字计数**（"read_file · × 3"），展开后才逐条显示（Step C 细化）

---

## 8 · Diff Block

### 块级 diff

```html
<div class="diff">
  <div class="diff__hunk">@@ src/agent/agent-loop.cpp · L142</div>
  <div class="diff__line diff__line--del">
    <span class="diff__gutter">142</span>
    <span>for (auto &memory_hit : memory_store_.search(prompt)) {</span>
  </div>
  <div class="diff__line diff__line--add">
    <span class="diff__gutter">142</span>
    <span>const auto &pre_rendered = context.memory_section;</span>
  </div>
</div>
```

### CSS

```css
.diff {
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  line-height: 1.74;
  background: var(--surface);
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
  overflow: hidden;
  margin: var(--space-3) 0;
}
.diff__hunk {
  padding: 6px var(--space-4);
  color: var(--content-soft);
  font-size: 11px;
  background: var(--surface-raised);
  border-bottom: 1px solid var(--rule-soft);
  letter-spacing: .02em;
}
.diff__line {
  display: grid;
  grid-template-columns: 56px 1fr;
  padding: 1px var(--space-4);
}
.diff__gutter {
  color: var(--content-soft);
  text-align: right;
  padding-right: var(--space-3);
  user-select: none;
}
.diff__line--add {
  background: color-mix(in oklch, var(--accent-tint) 60%, transparent);
}
.diff__line--add .diff__gutter::before {
  content: '+ '; color: var(--accent-deep);
}
.diff__line--del {
  background: color-mix(in oklch, var(--signal-tint) 60%, transparent);
}
.diff__line--del .diff__gutter::before {
  content: '- '; color: var(--signal-deep);
}
```

### 行内 diff

```html
<span class="idiff">
  把 <del>MAX_ITERATIONS=20</del><ins>MAX_ITERATIONS=25</ins> 并移除...
</span>
```

```css
.idiff { font-family: var(--font-body); line-height: 1.72; }
.idiff ins {
  background: color-mix(in oklch, var(--accent-tint) 85%, transparent);
  text-decoration: none;
  color: var(--accent-deep);
  padding: 1px 5px;
  border-radius: var(--radius-sm);
}
.idiff del {
  background: color-mix(in oklch, var(--signal-tint) 85%, transparent);
  text-decoration: line-through;
  text-decoration-color: var(--signal);
  color: var(--signal-deep);
  padding: 1px 5px;
  border-radius: var(--radius-sm);
}
```

### 规则
- Moss（accent）= 新增；Sienna（signal）= 删除 —— 一致绑定
- 行内 diff **必须**保留 `text-decoration: line-through` + 背景色 + 文字深色——三重冗余确保色盲可读
- 长 diff 自动折叠到 400px 高，rest 展开

---

## 9 · Reply-quote（引用块）

### 结构

```html
<div class="rq">
  <span class="rq__who">Re · lead</span>
  <span class="rq__text">帮我查一下 Anthropic 文档里 prompt-caching 的最新 TTL 限制</span>
</div>
```

### CSS

```css
.rq {
  font-family: var(--font-body);
  font-size: var(--text-ui);
  color: var(--content-muted);
  background: color-mix(in oklch, var(--surface-raised) 75%, transparent);
  padding: var(--space-3) var(--space-4);
  border-radius: var(--radius-md);
  margin: var(--space-2) 0;
  display: grid;
  grid-template-columns: auto 1fr;
  gap: var(--space-3);
  font-style: italic;
}
.rq__who {
  font-family: var(--font-mono);
  font-size: 11px;
  letter-spacing: .08em;
  color: var(--content-soft);
  font-style: normal;
  padding-top: 2px;
  white-space: nowrap;
}
```

### 规则
- 用于 teammate 回复引用 lead 的原始指令
- 斜体正文 + mono 署名 = "手写批注在便签上"的感觉
- 最多显示 2 行，超出 `text-overflow: ellipsis`

---

## 10 · Image Block

### 结构

```html
<figure class="image">
  <img src="..." alt="screenshot" class="image__img" />
  <figcaption class="image__caption">—— agent 附加的截图 / 生成图像</figcaption>
</figure>

<!-- 占位符（无图时） -->
<figure class="image image--placeholder">
  <div class="image__frame">IMAGE · 512 × 320</div>
  <figcaption class="image__caption">生成中 ...</figcaption>
</figure>
```

### CSS

```css
.image {
  margin: var(--space-3) 0;
  padding: var(--space-3);
  background: var(--surface);
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
  max-width: 420px;
}
.image__img {
  display: block;
  width: 100%;
  border-radius: var(--radius-sm);
}
.image__frame {
  border: 1px dashed var(--rule);
  border-radius: var(--radius-sm);
  height: 180px;
  display: flex;
  align-items: center;
  justify-content: center;
  background: repeating-linear-gradient(
    135deg,
    var(--surface-raised), var(--surface-raised) 6px,
    var(--surface) 6px, var(--surface) 12px
  );
  font-family: var(--font-mono);
  font-size: var(--text-caption);
  color: var(--content-soft);
  letter-spacing: .1em;
}
.image__caption {
  font-family: var(--font-body);
  font-style: italic;
  font-size: var(--text-ui);
  color: var(--content-muted);
  margin-top: var(--space-3);
  text-align: center;
}
```

---

## 11 · Math Block（占位 · KaTeX 接入在 Step C）

```html
<div class="math math--block">
  <span class="math__tag">Delimited Block Math</span>
  <!-- KaTeX 渲染注入这里 -->
</div>

<span class="math math--inline">Δt = 1 / f<sub>s</sub></span>
```

CSS 见 `typography.md · §7`。

---

## 12 · Orangutan Placeholder（主 agent 大头像 · 真实素材待设计）

```html
<div class="orang-placeholder">
  <span class="orang-placeholder__tag">Placeholder · Dynamic SVG</span>
  <span class="orang-placeholder__name"><em>Orangutan</em></span>
  <span>主 agent 头像 · 根据状态变化 · 真实素材待设计</span>
</div>
```

```css
.orang-placeholder {
  border: 1.5px dashed var(--rule);
  border-radius: var(--radius-lg);
  background:
    radial-gradient(ellipse at 30% 20%,
      oklch(92% 0.025 80 / .7), transparent 60%),
    repeating-linear-gradient(135deg,
      var(--surface-raised), var(--surface-raised) 8px,
      var(--surface) 8px, var(--surface) 16px);
  min-height: 240px;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  font-family: var(--font-mono);
  font-size: var(--text-meta);
  color: var(--content-muted);
  text-align: center;
  padding: var(--space-5);
  gap: var(--space-3);
}
.orang-placeholder__tag {
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: .16em;
  color: var(--content-soft);
}
.orang-placeholder__name {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 500;
  font-size: 30px;
  color: var(--content);
}
```

**注：**Orangutan 真实动态 SVG 由独立步骤（Step C 或专门的资产步骤）实现，现阶段**必须用本 placeholder**。禁止临时 AI 生成图标代替。

---

## 13 · 完整原子速查

| 原子 | 文件节 | 核心约束 |
|---|---|---|
| Button | §1 | 40px 高 · 不放大 · 5 种语义变体 |
| Badge | §2 | mono 11px · 胶囊 · 一屏 ≤8 |
| Field | §3 | 四边边框（禁左竖条） · focus 带 signal 光晕 |
| Avatar | §4 | 衬线斜体字母 · Sienna=lead / Moss=teammate |
| Message | §5 | 虚线分隔 · 不用气泡 · max-width 64ch |
| Thinking | §6 | 斜体 · 仅 lead 显示 · 可折叠 |
| Tool | §7 | 方块 glyph + mono · 220px 折叠 |
| Diff | §8 | Moss=+ / Sienna=- · 行内必须三重冗余 |
| Reply-quote | §9 | 斜体 + mono 署名 |
| Image | §10 | 外边框 + caption 斜体 |
| Math | §11 | KaTeX 注入 · 容器用 Newsreader 斜体 |
| Orangutan | §12 | 强制占位符 · 真实素材 TBD |
