# 拖拽上传 · Drag & Drop (Blot Overlay)

> 墨渍吸收覆盖层 · clip-path 扩散动画 · 多文件批量
> 视觉真值：`v-merged.html` · `.blot-overlay`

---

## 1 · 设计理念

拖拽文件时不是标准的"虚线框闪烁"，而是**墨渍吸收**——signal 色的覆盖层从 compose bar 中心以 `clip-path: circle()` 扩散开来，像墨滴落在纸面洇开。

---

## 2 · 覆盖层结构

```html
<div class="blot-overlay" aria-hidden="true">
  <span class="blot-overlay__glyph"><em>§</em></span>
  <span class="blot-overlay__label">松开即引用</span>
</div>
```

覆盖层覆盖整个 `.compose` 区域（`position: absolute; inset: 0`），compose bar 内容降至 `opacity: .25`。

---

## 3 · CSS

```css
.blot-overlay {
  position: absolute;
  inset: 0;
  background: var(--signal-tint);
  border: 2px solid var(--signal);
  border-radius: var(--radius-md);
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: var(--space-2);
  z-index: 20;
  animation: blot-spread 240ms var(--ease) both;
}

@keyframes blot-spread {
  from {
    opacity: 0;
    clip-path: circle(12% at 50% 50%);
  }
  to {
    opacity: 1;
    clip-path: circle(75% at 50% 50%);
  }
}

.blot-overlay__glyph {
  font-family: var(--font-display);
  font-style: italic;
  font-weight: 600;
  font-size: 28px;
  color: var(--signal);
}

.blot-overlay__label {
  font-family: var(--font-body);
  font-style: italic;
  font-size: var(--text-ui);
  color: var(--signal-deep);
}
```

**动画关键**：`clip-path: circle(12% → 75%)` 创造从中心扩散的墨渍效果。配合 240ms 和 `var(--ease)` 曲线，比 `opacity` 渐入有更强的"物理墨水"感。

---

## 4 · 拖拽状态机

```js
let dragCounter = 0;

// 监听整个 compose 区域
const composeEl = document.querySelector('.compose');

composeEl.addEventListener('dragenter', (e) => {
  e.preventDefault();
  dragCounter++;
  if (dragCounter === 1) {
    showBlotOverlay();
    fadeComposeBar(.25);
  }
});

composeEl.addEventListener('dragleave', (e) => {
  e.preventDefault();
  dragCounter--;
  if (dragCounter === 0) {
    hideBlotOverlay();
    fadeComposeBar(1);
  }
});

composeEl.addEventListener('dragover', (e) => {
  e.preventDefault();
  e.dataTransfer.dropEffect = 'copy';
});

composeEl.addEventListener('drop', (e) => {
  e.preventDefault();
  dragCounter = 0;
  hideBlotOverlay();
  fadeComposeBar(1);
  handleDroppedFiles(e.dataTransfer.files);
});
```

**`dragCounter` 的作用**：`dragenter`/`dragleave` 在子元素之间会触发冒泡，用计数器确保只在真正进入/离开 compose 区域时切换覆盖层。

---

## 5 · 文件处理

### 5.1 drop 处理

```js
function handleDroppedFiles(fileList) {
  const files = Array.from(fileList);

  // 过滤
  const valid = files.filter(f => {
    if (f.size > 10 * 1024 * 1024) {
      showToast(`${f.name} 超过 10 MB 限制`);
      return false;
    }
    return true;
  });

  // 逐个添加
  for (const file of valid) {
    const ref = addAttachment({
      type: 'file',
      name: file.name,
      size: file.size,
      file,
    });
    insertStickyNote(ref);
    // 拖拽不自动插入上标（隐式附件模式）
  }
}
```

### 5.2 文件大小限制

| 限制 | 值 | 处理 |
|---|---|---|
| 单文件最大 | 10 MB | 超过时 toast 提示，不添加 |
| 单次拖拽最大文件数 | 10 | 超过时只取前 10，toast 提示 |
| 总附件数上限 | 9 | 超过时 toast 提示，不添加 |

### 5.3 文件类型检测

不做前端文件类型限制（后端会验证），但可以在 `sticky__type` 里显示人类友好名称：

```js
function humanFileType(file) {
  const ext = file.name.split('.').pop().toLowerCase();
  const map = {
    png: '图片', jpg: '图片', jpeg: '图片', gif: '图片', webp: '图片', svg: '图片',
    pdf: 'PDF', doc: '文档', docx: '文档', txt: '文本',
    js: 'JS', ts: 'TS', cpp: 'C++', hpp: 'C++', py: 'Python', rs: 'Rust',
    json: 'JSON', yaml: 'YAML', md: 'Markdown',
  };
  return map[ext] || ext.toUpperCase();
}
```

---

## 6 · 全局拖拽监听

拖拽文件到浏览器窗口时，即使鼠标不在 compose 区域上方，也应提供视觉提示：

```js
// 在整个 chat-region 上监听
document.addEventListener('dragenter', (e) => {
  if (hasFiles(e)) {
    highlightComposeArea();
  }
});

document.addEventListener('dragleave', (e) => {
  if (e.relatedTarget === null) {
    // 鼠标离开浏览器窗口
    unhighlightComposeArea();
  }
});

function highlightComposeArea() {
  composeEl.classList.add('is-drop-hint');
}
```

```css
.compose.is-drop-hint .compose__bar {
  border-color: var(--signal);
  box-shadow: 0 0 0 2px oklch(from var(--signal) l c h / .1);
}
```

这是一个轻量提示（边框变 signal 色 + 外光晕），引导用户把文件拖到 compose 区域。当鼠标进入 compose 区域后，切换到完整的 blot-overlay。

---

## 7 · 覆盖层退出

```js
function hideBlotOverlay() {
  const overlay = composeEl.querySelector('.blot-overlay');
  if (!overlay) return;

  overlay.style.animation = 'blot-shrink 180ms var(--ease) forwards';
  overlay.addEventListener('animationend', () => overlay.remove(), { once: true });
}
```

```css
@keyframes blot-shrink {
  from {
    opacity: 1;
    clip-path: circle(75% at 50% 50%);
  }
  to {
    opacity: 0;
    clip-path: circle(8% at 50% 50%);
  }
}
```

退出时墨渍**收缩回去**，180ms（比扩散快一点），让"放下"的感觉利落。

---

## 8 · 无障碍

- `.blot-overlay` 必须 `aria-hidden="true"` —— 纯视觉装饰
- 拖拽文件添加后，屏幕阅读器通过 `aria-live="polite"` 的隐藏区域播报 `"已添加引用: {filename}"`
- 键盘用户通过 citation lookup 添加文件（§ 按钮 → 选择上传 → 弹出系统文件对话框）

---

## 9 · 性能约束

- blot-overlay 是**临时 DOM**——显示时创建，隐藏时移除
- `clip-path` 动画在现代浏览器上不触发 layout，但需要 GPU 合成
- 同时只可能有 **1 个** blot-overlay
- `dragCounter` 防止多次创建

---

## 10 · 禁止事项

- ❌ 使用虚线边框 + 闪烁做拖拽提示 —— 标准 AI-slop
- ❌ 覆盖层用 `opacity` 渐入替代 `clip-path` —— 失去墨渍扩散效果
- ❌ 拖拽时显示文件缩略图预览 —— 过度设计
- ❌ 拖拽区域限制为某个小区域 —— 整个 compose 区都可以 drop
- ❌ drop 后自动打开 citation lookup —— drop 是直接添加，不需要再查找
