# 表情贴图 · Reactions (Margin Annotations)

> 边栏批注风格 —— 左侧 2px 竖线 + 纯文字，像手稿页边的铅笔批注
> 视觉真值：`v-merged.html` · `.reactions` / `.reaction` / `.reaction-picker`

---

## 1 · 设计理念

表情贴图不是 Slack/Discord 的胶囊徽章，而是**手稿页边的铅笔批注**：
- 左侧一条细线（2px rule-soft）连接所有批注
- 每个 reaction 是纯文字（icon + count），无背景色、无胶囊、无描边
- 激活状态用斜体 + Sienna 下划线标记，像用红笔划重点
- 整体视觉重量极轻，不打断消息阅读流

---

## 2 · 结构

### 2.1 Reactions 容器

```html
<div class="reactions">
  <span class="reaction reaction--active">
    <span class="reaction__icon">✅</span>
    <span class="reaction__count">1</span>
  </span>
  <span class="reaction">
    <span class="reaction__icon">📚</span>
    <span class="reaction__count">1</span>
  </span>
</div>
```

### 2.2 添加按钮

每条消息右上角，hover 时显示铅笔图标：

```html
<div class="msg msg--team" style="position:relative;">
  <!-- ... msg__head, msg__body ... -->
  <div class="reactions">...</div>
  <button class="msg__react-btn" aria-label="添加表情">✎</button>
</div>
```

### 2.3 Picker 弹层

点击 `msg__react-btn` 后弹出：

```html
<div class="reaction-picker is-open">
  <button class="reaction-picker__item">👍</button>
  <button class="reaction-picker__item">🔥</button>
  <button class="reaction-picker__item">💡</button>
  <button class="reaction-picker__item">✅</button>
  <button class="reaction-picker__item">📚</button>
  <button class="reaction-picker__item">🚀</button>
  <button class="reaction-picker__item">❓</button>
  <button class="reaction-picker__item">👀</button>
</div>
```

---

## 3 · CSS

### 3.1 Reactions 容器

```css
.reactions {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-top: var(--space-2);
  padding-left: var(--space-3);
  border-left: 2px solid var(--rule-soft);
}
```

**关键**：`border-left: 2px solid var(--rule-soft)` 是 margin annotation 的视觉签名——一条细竖线把 reactions 和正文隔开，像手稿的边栏。

### 3.2 单个 Reaction

```css
.reaction {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 1px 6px 1px 4px;
  border: none;
  border-radius: var(--radius-sm);
  background: transparent;
  font-family: var(--font-mono);
  font-size: 11px;
  color: var(--content-muted);
  cursor: pointer;
  transition: color var(--duration-fast) var(--ease),
              background var(--duration-fast) var(--ease);
  user-select: none;
}

.reaction:hover {
  background: var(--surface-raised);
  color: var(--content);
}

.reaction--active {
  color: var(--signal-deep);
  font-style: italic;
}

.reaction--active .reaction__icon {
  border-bottom: 1px solid var(--signal);
}
```

**激活态三要素**：
1. 文字变 `--signal-deep`（赭红深色）
2. 整体变斜体（`font-style: italic`）
3. icon 下加 `1px solid var(--signal)` 下划线

三者缺一不可——这是"红笔划重点"的视觉隐喻。

### 3.3 Icon 与 Count

```css
.reaction__icon {
  font-size: 13px;
  line-height: 1;
  font-family: var(--font-body);  /* emoji 用系统字体 */
  padding-bottom: 1px;
}

.reaction__count {
  font-size: 10px;
  font-weight: 500;
  opacity: .7;
}
```

### 3.4 添加按钮

```css
.msg__react-btn {
  position: absolute;
  top: var(--space-3);
  right: 0;
  width: 24px;
  height: 24px;
  border: none;
  border-radius: var(--radius-sm);
  background: transparent;
  color: var(--content-soft);
  cursor: pointer;
  display: flex;
  align-items: center;
  justify-content: center;
  font-family: var(--font-mono);
  font-size: 14px;
  opacity: 0;
  transition: opacity var(--duration-fast) var(--ease),
              color var(--duration-fast) var(--ease);
}

.msg:hover .msg__react-btn { opacity: 1; }
.msg__react-btn:hover { color: var(--content-muted); }
```

### 3.5 Picker 弹层

```css
.reaction-picker {
  position: absolute;
  bottom: calc(100% + 6px);
  right: 0;
  display: flex;
  gap: 2px;
  padding: 4px 6px;
  background: var(--surface);
  border: 1px solid var(--rule);
  border-radius: var(--radius-md);
  box-shadow: var(--shadow-2);
  z-index: 10;
  opacity: 0;
  pointer-events: none;
  transform: translateY(4px);
  transition: opacity var(--duration-fast) var(--ease),
              transform var(--duration-fast) var(--ease);
}

.reaction-picker.is-open {
  opacity: 1;
  pointer-events: auto;
  transform: translateY(0);
}

.reaction-picker__item {
  width: 30px;
  height: 30px;
  display: flex;
  align-items: center;
  justify-content: center;
  border: none;
  background: transparent;
  border-radius: var(--radius-sm);
  font-size: 16px;
  cursor: pointer;
  transition: background var(--duration-fast) var(--ease);
}

.reaction-picker__item:hover {
  background: var(--surface-raised);
}
```

---

## 4 · 交互逻辑

### 4.1 添加 Reaction

```js
function addReaction(msgId, emoji) {
  const existing = findReaction(msgId, emoji);
  if (existing) {
    // 已存在 → 增加计数 + 标记 active
    incrementCount(existing);
    markActive(existing);
  } else {
    // 不存在 → 新建 reaction
    createReaction(msgId, emoji, 1);
  }
}
```

### 4.2 移除 Reaction（再次点击已 active 的 reaction）

```js
function removeReaction(msgId, emoji) {
  const existing = findReaction(msgId, emoji);
  if (!existing) return;
  
  const count = getCount(existing);
  if (count <= 1) {
    // 计数归零 → 移除 DOM 节点
    existing.remove();
  } else {
    // 减少计数 + 取消 active
    decrementCount(existing);
    markInactive(existing);
  }
}
```

### 4.3 Picker 开关

```js
function togglePicker(btn) {
  // 关闭已有的 picker
  closeAllPickers();
  
  // 创建新 picker
  const picker = createPickerElement();
  btn.parentElement.appendChild(picker);
  
  // 点击外部关闭
  const closeHandler = (e) => {
    if (!picker.contains(e.target) && e.target !== btn) {
      picker.remove();
      document.removeEventListener('click', closeHandler);
    }
  };
  setTimeout(() => document.addEventListener('click', closeHandler), 10);
}
```

---

## 5 · Emoji 列表

默认 picker 提供 **8 个** emoji，按使用频率排序：

| 位置 | Emoji | 语义 |
|---|---|---|
| 1 | 👍 | 认同 / 好的 |
| 2 | 🔥 | 重要 / 突出 |
| 3 | 💡 | 有启发 / 好想法 |
| 4 | ✅ | 完成 / 确认 |
| 5 | 📚 | 参考来源 |
| 6 | 🚀 | 快 / 高效 |
| 7 | ❓ | 疑问 / 不理解 |
| 8 | 👀 | 关注 / 正在看 |

**不提供自定义 emoji 输入**——保持简洁，避免 emoji 键盘的复杂度。后续版本可扩展。

---

## 6 · 数据模型

```ts
interface Reaction {
  emoji: string;       // "👍" | "🔥" | ...
  count: number;       // ≥ 1
  isActive: boolean;   // 当前用户是否已添加
}

interface MessageReactions {
  msgId: string;
  reactions: Reaction[];
}
```

API 绑定（预留）：

```
POST /api/v1/chat/reactions
  { msg_id, emoji, action: "add" | "remove" }
```

---

## 7 · 性能约束

- 每条消息 reactions ≤ **8 个**不同 emoji
- 单个 reaction count 显示上限 **99**，超过显示 "99+"
- picker 弹层是**临时 DOM**，关闭时移除，不用 `display: none` 缓存
- reactions 容器 `flex-wrap: wrap`，不设固定高度

---

## 8 · 无障碍

- `.msg__react-btn` 必须有 `aria-label="添加表情"`
- `.reaction-picker__item` 必须有 `aria-label`（如 `aria-label="点赞"`）
- picker 打开时焦点移入第一个 item
- Esc 关闭 picker，焦点回到 `msg__react-btn`
- `prefers-reduced-motion: reduce` 下，picker 的 `transform` 过渡禁用

---

## 9 · 禁止事项

- ❌ 胶囊徽章 / pill badge / 圆角背景色块 —— 这是 Slack/Discord 的风格
- ❌ reaction icon 大于 14px —— 太大视觉重量超标
- ❌ picker 用 emoji 键盘 / 搜索框 —— 过度设计
- ❌ reactions 容器加背景色 —— 必须透明，只有左侧竖线
- ❌ active 状态用 `--signal-tint` 背景 —— 只用文字色 + 斜体 + 下划线
- ❌ hover 时 icon 放大（scale） —— 只允许背景色变化
