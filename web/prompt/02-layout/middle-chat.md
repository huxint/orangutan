# 中栏 · Chat Region

> flex column · 上 chat-scroll 下 chat-footer · 内部读取列 max-width 720
> 视觉真值：`v1-column.html` · `.chat-region`

---

## 1 · 外壳

```html
<section class="chat-region" data-chat-state="idle">
  <div class="chat-scroll" id="chat-scroll">
    <div class="chat-scroll__inner">
      <!-- 消息流 · Step C 细化渲染 -->
    </div>
  </div>
  <footer class="chat-footer">
    <div class="chat-footer__inner">
      <div class="status-row" aria-live="polite">...</div>
      <form class="send-bar" onsubmit="handleSend(event)">...</form>
    </div>
  </footer>
</section>
```

```css
.chat-region {
  display: flex;
  flex-direction: column;
  min-width: 0;              /* 防止 grid 项撑破 */
  overflow: hidden;
  background: var(--surface);
}
.chat-scroll {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
  padding: var(--space-6) var(--space-5);
  scroll-behavior: auto;     /* 仅在显式跳转启用 smooth */
}
.chat-scroll__inner {
  max-width: 720px;
  margin: 0 auto;
}
.chat-footer {
  flex-shrink: 0;
  padding: var(--space-3) var(--space-5) var(--space-4);
  border-top: 1px solid var(--rule-soft);
  background: var(--surface);
}
.chat-footer__inner {
  max-width: 720px;
  margin: 0 auto;
}
```

**硬约束**：`chat-footer__inner` 的 `max-width` **必须等于** `chat-scroll__inner` 的 `max-width`（两处同步调整）。

---

## 2 · Chat Scroll 的滚动行为

### 2.1 Sticky-to-bottom（**禁 scrollIntoView**）

```js
const scrollEl = document.getElementById('chat-scroll');
const AUTO_STICK_THRESHOLD = 72;   // 距底部 ≤72px 视为"贴底"

function isNearBottom(el) {
  return el.scrollHeight - el.scrollTop - el.clientHeight <= AUTO_STICK_THRESHOLD;
}

function maybeStickToBottom(el) {
  const near = isNearBottom(el);
  requestAnimationFrame(() => {
    if (near) {
      // ⚠️ 绝不使用 el.lastChild.scrollIntoView()
      el.scrollTop = el.scrollHeight;
    }
  });
}
```

**为何禁 `scrollIntoView`**：它会把最近可滚动祖先也滚起来，破坏整页 layout —— 这是 AI-slop 最常见的"滚动失控"bug。

### 2.2 "回到底部" 浮按钮

```html
<button class="back-to-bottom" type="button" aria-label="回到最新消息" data-visible="false">
  <span class="back-to-bottom__count">3</span>
  <span aria-hidden="true">↓</span>
</button>
```
```css
.back-to-bottom {
  position: sticky;
  bottom: var(--space-4);
  margin-left: auto;
  margin-right: 0;
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 6px 10px 6px 12px;
  background: var(--content);
  color: var(--surface);
  border: none;
  border-radius: 999px;
  box-shadow: var(--shadow-2);
  font-family: var(--font-mono);
  font-size: 11px;
  cursor: pointer;
  opacity: 0;
  transition: opacity var(--duration-med) var(--ease);
  pointer-events: none;
}
.back-to-bottom[data-visible="true"] {
  opacity: 1;
  pointer-events: auto;
}
.back-to-bottom__count {
  background: var(--signal-tint);
  color: var(--signal-deep);
  padding: 1px 6px;
  border-radius: 999px;
}
```

逻辑：
```js
scrollEl.addEventListener('scroll', () => {
  const shouldShow = !isNearBottom(scrollEl) && unreadCount > 0;
  backBtn.setAttribute('data-visible', shouldShow ? 'true' : 'false');
});
backBtn.addEventListener('click', () => {
  scrollEl.scrollTo({ top: scrollEl.scrollHeight, behavior: 'smooth' });
  backBtn.setAttribute('data-visible', 'false');
  unreadCount = 0;
});
```

### 2.3 历史加载（向上滚）

```js
scrollEl.addEventListener('scroll', () => {
  if (scrollEl.scrollTop < 120 && !loadingHistory && hasMoreHistory) {
    const prevHeight = scrollEl.scrollHeight;
    loadHistory().then(() => {
      // 维持视觉位置
      scrollEl.scrollTop = scrollEl.scrollHeight - prevHeight;
    });
  }
});
```

---

## 3 · 消息占位（Step C 替换）

本步骤只保证消息容器**存在且样式正确**，具体消息块结构由 Step C 定义。

占位：
```html
<div class="chat-scroll__inner">
  <div class="msg">
    <div class="msg__avatar"><em>H</em></div>
    <div>
      <div class="msg__head"><span class="msg__name">Huxint</span></div>
      <div class="msg__body"><p>...</p></div>
    </div>
  </div>
</div>
```

本步骤用 `v1-column.html` 里的 `.msg` CSS 作临时实现。

---

## 4 · Status Row

```html
<div class="status-row" aria-live="polite">
  <span class="item">
    <span class="dot-blink" aria-hidden="true"></span>
    <span data-slot="primary">lead 思考中 · 3s</span>
  </span>
  <span class="sep" aria-hidden="true">·</span>
  <span class="item" data-slot="tools">3 tools running</span>
  <span class="sep" aria-hidden="true">·</span>
  <span class="item" data-slot="teammates">2 teammates active</span>
  <span class="sep" aria-hidden="true">·</span>
  <span class="item" data-slot="tokens">2,431 / 200k tokens</span>
  <span class="item" data-slot="hint" style="margin-left:auto;">Esc 中断</span>
</div>
```

```css
.status-row {
  display: flex;
  align-items: center;
  gap: var(--space-3);
  padding: 0 var(--space-1) var(--space-2);
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--content-muted);
  flex-wrap: wrap;
}
.status-row .item {
  display: flex;
  align-items: center;
  gap: 6px;
}
.status-row .dot-blink {
  width: 6px; height: 6px;
  border-radius: 50%;
  background: var(--accent);
  animation: pulse 1.6s ease-in-out infinite;
}
.status-row .sep { color: var(--rule); }
```

### 4.1 信息层级

从左到右优先级，**最多 5 项**（含 Hint）：

| 位置 | 内容 | 条件 |
|---|---|---|
| 1 | Primary 状态：`{agent} {动作} · {elapsed}` | 永远显示 |
| 2 | Tools：`N tools running` / `无工具运行` | 永远显示 |
| 3 | Teammates：`N teammates active` | N>0 显示 |
| 4 | Tokens：`used / total` | 永远显示；>75% `used` 变 signal-deep |
| 5 | Hint：`Esc 中断` / `Enter 发送` | 右对齐 · 永远显示 |

### 4.2 必须遵守

- 总量 **≤ 5 项**
- 每项文字 **≤ 24 字**
- `aria-live="polite"` — 不抢屏幕阅读器焦点
- 不用颜色突出关键信息（除 token 超额）

---

## 5 · Send Bar

```html
<form class="send-bar" onsubmit="handleSend(event)">
  <button type="button" class="send-bar__attach" aria-label="添加附件">+</button>
  <textarea
    class="send-bar__input"
    rows="1"
    placeholder="对 lead 说点什么 · Shift+Enter 换行"
    aria-label="消息输入"
  ></textarea>
  <button type="submit" class="send-bar__send">
    发送 <span class="kbd" aria-hidden="true">⏎</span>
  </button>
</form>
```

```css
.send-bar {
  display: grid;
  grid-template-columns: auto 1fr auto;
  gap: var(--space-2);
  align-items: end;
  padding: var(--space-3);
  background: var(--surface-raised);
  border: 1px solid var(--rule-soft);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-1);
}

.send-bar__attach {
  width: 36px; height: 36px;
  border-radius: var(--radius-sm);
  background: transparent;
  border: 1px solid var(--rule);
  color: var(--content-muted);
  cursor: pointer;
  display: flex;
  align-items: center; justify-content: center;
  font-family: var(--font-mono);
  font-size: 16px;
}
.send-bar__attach:hover {
  background: var(--surface);
  color: var(--content);
}

.send-bar__input {
  font-family: var(--font-body);
  font-size: var(--text-body);
  line-height: 1.5;
  background: transparent;
  border: none;
  outline: none;
  color: var(--content);
  padding: 8px 4px;
  resize: none;
  min-height: 36px;
  max-height: 180px;
  overflow-y: auto;
}
.send-bar__input::placeholder {
  color: var(--content-soft);
  font-style: italic;
}

.send-bar__send {
  height: 36px;
  padding: 0 16px;
  border-radius: var(--radius-sm);
  background: var(--signal);
  color: var(--surface);
  border: none;
  font-family: var(--font-body);
  font-weight: 500;
  font-size: var(--text-ui);
  cursor: pointer;
  display: flex;
  align-items: center;
  gap: 6px;
}
.send-bar__send:hover { background: var(--signal-deep); }
.send-bar__send:disabled {
  background: var(--surface-sunken);
  color: var(--content-soft);
  cursor: not-allowed;
}
.send-bar__send .kbd {
  font-family: var(--font-mono);
  font-size: 10px;
  opacity: .7;
}
```

### 5.1 Textarea 自动增高

```js
const input = document.querySelector('.send-bar__input');
input.addEventListener('input', () => {
  input.style.height = 'auto';
  input.style.height = Math.min(input.scrollHeight, 180) + 'px';
});
```

### 5.2 Enter 发送 / Shift+Enter 换行

```js
input.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    sendMessage();
  }
});
```

**必须** `!e.isComposing` —— 中文输入法 IME 冲突防护。

### 5.3 发送状态机

挂在 `<section class="chat-region" data-chat-state="...">`：

| `data-chat-state` | 含义 | 视觉 |
|---|---|---|
| `idle` | 等待用户输入 | send 可用 · hint "Enter 发送" |
| `composing` | 用户在输入（textarea 有值） | send 高亮 · hint "Enter 发送" |
| `in-flight` | chat SSE 流进行中 | send 禁用 · hint "Esc 中断" · status dot 亮 |
| `error` | 最近发送失败 | send 恢复 · status 显示错误 · 输入保留 |

### 5.4 Esc 中断

```js
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    const state = document.querySelector('.chat-region').getAttribute('data-chat-state');
    if (state === 'in-flight') {
      abortChat();
    }
  }
});
```

---

## 6 · 附件入口（本步骤仅占位）

点击 `.send-bar__attach` 展开附件面板（Step F 细化）：
- 上传文件 · 引用会话片段 · 粘贴 URL · 插入 workspace 文件

本步骤 `onclick` 先 `console.log('附件 · Step F')`。

---

## 7 · API 绑定

### 7.1 发送消息

```js
async function sendMessage() {
  const message = input.value.trim();
  if (!message) return;
  input.value = '';
  input.style.height = 'auto';
  setChatState('in-flight');

  try {
    const res = await fetch('/api/v1/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        message,
        agent_key: currentAgentKey,   // 'default' / 'lead'
        session_id: currentSessionId, // 可为空
      }),
    });
    // SSE 流解析由 Step C 接管
    handleSSEStream(res.body);
  } catch (err) {
    setChatState('error');
    input.value = message;
  }
}
```

### 7.2 中断

```js
async function abortChat() {
  if (!currentSessionId) return;
  await fetch('/api/v1/chat/abort', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ session_id: currentSessionId }),
  });
  setChatState('idle');
}
```

### 7.3 状态切换工具函数

```js
function setChatState(state) {
  document.querySelector('.chat-region').setAttribute('data-chat-state', state);
  const sendBtn = document.querySelector('.send-bar__send');
  sendBtn.disabled = (state === 'in-flight');
  updateStatusRow(state);
}
```

---

## 8 · 禁止事项

- ❌ 使用 `scrollIntoView` —— 任何地方
- ❌ 把 send-bar 做成浮卡（浮卡是 V2 的风格，V1 永远 dock）
- ❌ Status row 超过 5 项
- ❌ 把消息列表渲染成 table / flex-wrap（只用垂直 flex / grid 行）
- ❌ 默认 textarea placeholder 是 emoji 或斜杠指令（Step I 才加 slash command）
- ❌ 主题切换时中栏闪烁

---

## 9 · 自检

- [ ] `body` 不滚动，只有 `.chat-scroll` 滚动
- [ ] 新消息贴底时 stick，用户上滚时不强拉回
- [ ] Shift+Enter 换行，Enter 发送，IME 输入不误触
- [ ] textarea 最多 180px 高，之后内部滚动
- [ ] 中文输入法可用（`isComposing` 生效）
- [ ] `data-chat-state` 正确切换 idle / composing / in-flight / error
- [ ] status-row 有 `aria-live="polite"`
- [ ] Esc 在 in-flight 下触发 abort
- [ ] 回到底部浮按钮在上滚时淡入、到底时淡出
- [ ] **全项目 grep 不到 `scrollIntoView`**
