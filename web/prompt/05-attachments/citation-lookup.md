# Citation Lookup · § 引用查找器

> 点 § 或输入 @ 唤出的浮层 · autocomplete 风格 · 统一搜索三类源
> 视觉真值：`v-merged.html` · `.cite-lookup`

---

## 1 · § 按钮

### 1.1 结构

```html
<button class="compose__cite" type="button" aria-label="插入引用" aria-expanded="false">§</button>
```

§ 按钮替代 Step 02 中 `.send-bar__attach` 的通用 `+`。位于 compose bar 最左侧，和 textarea、发送按钮同行。

### 1.2 CSS

```css
.compose__cite {
  width: 36px;
  height: 36px;
  border-radius: var(--radius-sm);
  background: transparent;
  border: 1px solid var(--rule);
  color: var(--content-muted);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 18px;
  transition: all var(--duration-fast) var(--ease);
}

.compose__cite:hover {
  background: var(--surface);
  color: var(--content);
}

.compose__cite.is-active {
  border-color: var(--signal);
  color: var(--signal-deep);
  background: var(--signal-tint);
}
```

**注意**：§ 字形用 `font-display`（Newsreader）italic，不用 mono——它是一个"签名符号"，不是代码。

### 1.3 状态

| 状态 | class | 视觉 | `aria-expanded` |
|---|---|---|---|
| 默认 | 无 | 灰边框 · muted 色 § | `false` |
| hover | `:hover` | 浅底 · 深色 § | `false` |
| 激活（lookup 打开） | `.is-active` | signal-tint 底 · signal-deep § | `true` |

---

## 2 · Lookup 浮层结构

```html
<div class="cite-lookup" role="listbox" aria-label="引用查找">
  <div class="cite-lookup__head">
    <span class="cite-lookup__glyph" aria-hidden="true"><em>§</em></span>
    <input
      class="cite-lookup__field"
      type="text"
      placeholder="搜索文件、工作区、链接…"
      role="combobox"
      aria-expanded="true"
      aria-controls="cite-results"
      aria-activedescendant=""
      autocomplete="off"
    />
    <span class="cite-lookup__hint" aria-hidden="true">↑↓ 导航 · Enter 选中 · Esc 关闭</span>
  </div>
  <div id="cite-results">
    <div class="cite-cat">上传</div>
    <div class="cite-item is-focused" role="option" id="cite-upload" aria-selected="true">
      <span class="cite-item__glyph"><em>f</em></span>
      <span class="cite-item__label">选择本地文件…</span>
      <span class="cite-item__meta">拖拽或点击</span>
    </div>
    <div class="cite-cat">工作区</div>
    <div class="cite-item" role="option" id="cite-ws-1">
      <span class="cite-item__glyph"><em>w</em></span>
      <span class="cite-item__label">src/agent/agent-loop.hpp</span>
      <span class="cite-item__meta">4.2 kB</span>
    </div>
    <!-- ... more items ... -->
    <div class="cite-cat">链接</div>
    <div class="cite-item" role="option" id="cite-url">
      <span class="cite-item__glyph"><em>u</em></span>
      <span class="cite-item__label">粘贴 URL…</span>
      <span class="cite-item__meta">Enter 确认</span>
    </div>
  </div>
</div>
```

---

## 3 · Lookup CSS

```css
.cite-lookup {
  position: absolute;
  bottom: calc(100% + 6px);
  left: 0;
  right: 0;
  background: var(--surface);
  border: 1px solid var(--rule);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-2);
  z-index: 10;
  overflow: hidden;
  animation: cite-rise 220ms var(--ease) both;
}

@keyframes cite-rise {
  from { opacity: 0; transform: translateY(6px); }
  to   { opacity: 1; transform: translateY(0); }
}
```

### 3.1 Head

```css
.cite-lookup__head {
  display: flex;
  align-items: center;
  gap: var(--space-2);
  padding: var(--space-3);
  border-bottom: 1px solid var(--rule-soft);
}

.cite-lookup__glyph {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 16px;
  color: var(--signal);
  flex-shrink: 0;
  width: 20px;
  text-align: center;
}

.cite-lookup__field {
  flex: 1;
  border: none;
  background: transparent;
  font-family: var(--font-mono);
  font-size: 13px;
  color: var(--content);
  outline: none;
}
.cite-lookup__field::placeholder {
  color: var(--content-soft);
}

.cite-lookup__hint {
  font-family: var(--font-mono);
  font-size: 10px;
  color: var(--content-soft);
  white-space: nowrap;
}
```

### 3.2 Category Header

```css
.cite-cat {
  padding: 5px var(--space-3) 4px;
  font-family: var(--font-mono);
  font-size: 10px;
  text-transform: uppercase;
  letter-spacing: .1em;
  color: var(--content-soft);
  background: var(--surface-sunken);
}
```

### 3.3 Result Item

```css
.cite-item {
  display: grid;
  grid-template-columns: 20px 1fr auto;
  gap: var(--space-2);
  align-items: center;
  padding: 7px var(--space-3);
  cursor: pointer;
  font-family: var(--font-mono);
  font-size: 12px;
  color: var(--content-muted);
  transition: background var(--duration-fast) var(--ease);
}

.cite-item:hover {
  background: var(--surface-raised);
}

.cite-item.is-focused {
  background: var(--signal-tint);
  color: var(--content);
}

.cite-item__glyph {
  font-family: var(--font-display);
  font-style: italic;
  font-size: 14px;
  color: var(--content-soft);
  text-align: center;
}
.cite-item.is-focused .cite-item__glyph {
  color: var(--signal-deep);
}

.cite-item__label {
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.cite-item__meta {
  font-size: 10px;
  color: var(--content-soft);
  white-space: nowrap;
}

.cite-item + .cite-item {
  border-top: 1px solid var(--rule-soft);
}
.cite-cat + .cite-item {
  border-top: none;
}
```

---

## 4 · Glyph 系统

每种来源用一个 Newsreader italic 小写字母作标识（不用 emoji，不用图标库）：

| 来源 | glyph | 含义 |
|---|---|---|
| 本地上传 | *f* | file |
| 工作区文件 | *w* | workspace |
| URL 链接 | *u* | url |

glyph 放在 `.cite-item__glyph` 里，`.cite-item.is-focused` 时变 `signal-deep` 色。

---

## 5 · 键盘导航

```js
const lookup = document.querySelector('.cite-lookup');
const items = () => lookup.querySelectorAll('.cite-item');
let focusIdx = 0;

function moveFocus(delta) {
  const list = items();
  list[focusIdx]?.classList.remove('is-focused');
  focusIdx = Math.max(0, Math.min(list.length - 1, focusIdx + delta));
  const target = list[focusIdx];
  target?.classList.add('is-focused');
  target?.setAttribute('aria-selected', 'true');
  // 更新 combobox 的 aria-activedescendant
  field.setAttribute('aria-activedescendant', target?.id || '');
}

field.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowDown') { e.preventDefault(); moveFocus(1); }
  if (e.key === 'ArrowUp')   { e.preventDefault(); moveFocus(-1); }
  if (e.key === 'Enter')     { e.preventDefault(); selectItem(focusIdx); }
  if (e.key === 'Escape')    { closeLookup(); }
});
```

**快捷键总结**：

| 键 | 行为 |
|---|---|
| ↑ / ↓ | 在结果项间移动焦点 |
| Enter | 选中当前焦点项 |
| Escape | 关闭 lookup，焦点回 § 按钮 |
| 任意字符 | 过滤结果（实时搜索） |

---

## 6 · 搜索过滤

### 6.1 过滤逻辑

```js
field.addEventListener('input', () => {
  const q = field.value.trim().toLowerCase();
  if (!q) {
    showDefaultResults();
    return;
  }

  // 1. 如果是 URL 格式 → 只显示 URL 输入项
  if (isUrlLike(q)) {
    showUrlEntry(q);
    return;
  }

  // 2. 否则 → fuzzy match 工作区文件
  const matches = fuzzyMatchWorkspaceFiles(q);
  renderFilteredResults(matches);
  // 上传项和 URL 项始终保留在最后
});
```

### 6.2 默认结果

lookup 刚打开时的默认结果列表：

1. **上传**类别：`选择本地文件…`（1 项，永远在最前）
2. **工作区**类别：最近修改的 5 个文件（从 `/api/v1/workspace/files` 获取）
3. **链接**类别：`粘贴 URL…`（1 项，永远在最后）

### 6.3 工作区文件数据

```js
async function loadWorkspaceFiles() {
  const res = await fetch('/api/v1/workspace/files');
  return res.json();
  // → [{ path: "src/agent/agent-loop.hpp", size: 4200, modified: "..." }, ...]
}
```

搜索匹配：对 `path` 做子串匹配（不区分大小写），匹配到的子串用 `<strong>` 高亮。

---

## 7 · 选中行为

### 7.1 选择工作区文件

```js
function selectWorkspaceFile(filePath, fileSize) {
  const ref = addAttachment({ type: 'workspace', path: filePath, size: fileSize });
  insertStickyNote(ref);
  insertSupMarker(ref.index);
  closeLookup();
}
```

### 7.2 选择"上传文件"

```js
function selectUploadEntry() {
  closeLookup();
  openFileDialog();  // 弹出系统文件选择对话框
}
```

文件选择后：
```js
fileInput.addEventListener('change', (e) => {
  for (const file of e.target.files) {
    const ref = addAttachment({ type: 'file', name: file.name, size: file.size, file });
    insertStickyNote(ref);
    insertSupMarker(ref.index);
  }
});
```

### 7.3 选择"URL"

如果搜索框内容是 URL 格式，Enter 直接添加：

```js
function selectUrlEntry(url) {
  const ref = addAttachment({ type: 'url', url });
  insertStickyNote(ref);
  insertSupMarker(ref.index);
  closeLookup();
}
```

如果搜索框非 URL，聚焦到 URL 项后按 Enter → 清空搜索框并切换 placeholder 为 `https://...`，等待粘贴。

---

## 8 · 触发方式

两种方式打开 lookup，**同时保留**：

| 方式 | 触发 | 行为 |
|---|---|---|
| 点击 § 按钮 | click | 打开 lookup，焦点移入搜索框 |
| 在 textarea 输入 `@` | keydown | 打开 lookup，`@` 不写入 textarea，焦点移入搜索框 |

### 8.1 @ 触发逻辑

```js
textarea.addEventListener('keydown', (e) => {
  if (e.key === '@' && !e.isComposing) {
    e.preventDefault();
    openLookup();
  }
});
```

**必须** `!e.isComposing` —— 中文输入法 IME 冲突防护，和 Step 02 send bar 的 Enter 防护逻辑一致。

---

## 9 · 打开 / 关闭状态机

```js
let lookupOpen = false;

function openLookup() {
  if (lookupOpen) return;
  lookupOpen = true;

  citeBtn.classList.add('is-active');
  citeBtn.setAttribute('aria-expanded', 'true');

  const lookup = createLookupElement();
  composeEl.appendChild(lookup);

  // 焦点移入搜索框
  lookup.querySelector('.cite-lookup__field').focus();

  // 加载默认结果
  loadDefaultResults();
}

function closeLookup() {
  if (!lookupOpen) return;
  lookupOpen = false;

  citeBtn.classList.remove('is-active');
  citeBtn.setAttribute('aria-expanded', 'false');

  composeEl.querySelector('.cite-lookup')?.remove();

  // 焦点回到 textarea
  textarea.focus();
}
```

**点击外部关闭**：

```js
document.addEventListener('mousedown', (e) => {
  if (lookupOpen && !composeEl.contains(e.target)) {
    closeLookup();
  }
});
```

---

## 10 · 禁止事项

- ❌ lookup 做成模态弹窗 / dialog —— 必须是无模态浮层
- ❌ 结果项用图标库（Lucide / Heroicons）—— 只用斜体字母 glyph
- ❌ 分类头用 signal/accent 色 —— 永远 `content-soft` + `surface-sunken`
- ❌ 搜索框自带清除按钮（X icon）—— Esc 关闭整个 lookup 即可
- ❌ 结果超过 10 项时分页 —— 截断到 10 项 + 显示 `+N more…` 文本
- ❌ lookup 宽度超出 compose bar —— 必须 `left: 0; right: 0` 对齐
