# 护栏 · Guardrails

> 必须遵守的"不要这样做"清单、完工自检表、常见陷阱。
> 任何 PR 合入前都应跑一遍 §4 的自检。

---

## 1 · 字体护栏

### ❌ 绝不出现的字体族
任何 CSS 里出现以下字符串，视为违反：

| 字符串 | 原因 |
|---|---|
| `Inter` | 被 AI 工具 UI 过度使用；一看就是"生成的" |
| `Roboto` | Google 默认；与 Android 品牌绑定 |
| `Arial` | 系统退路字；视觉无个性 |
| `Fraunces` | 过度使用的 display 字族 |
| `system-ui` / `-apple-system` / `BlinkMacSystemFont` | 系统默认；视觉不可控 |
| `Helvetica` / `Helvetica Neue` | 泛化；无品牌辨识度 |
| `sans-serif`（裸用） | 必须走 `--font-mono` 或 `--font-body` 的 fallback 链 |

### ✅ 只允许
- `var(--font-body)` 或 `var(--font-display)` → 解析为 **Newsreader**（带 fallback）
- `var(--font-mono)` → 解析为 **Spline Sans Mono**（带 fallback）

Grep 检查命令：
```bash
rg "Inter|Roboto|Arial|Fraunces|system-ui|-apple-system|BlinkMacSystemFont|Helvetica" src/ web/
# 期望无结果（除本文件和 design HTML 历史注释）
```

---

## 2 · 颜色护栏

### ❌ 禁止
- 硬编码 `#hex`、`rgb(...)`、`hsl(...)` —— 全部走 `var(--...)`
- 在组件样式中**直接**用 `oklch(...)` —— 仅 `tokens.css` 允许
- 凭空发明新颜色 —— 需要新色时先扩展 tokens（并征得设计 review）
- 使用 `filter: hue-rotate(...)` 模拟变色 —— 破坏 token 追踪

### ✅ 允许
- `var(--signal)` / `var(--accent)` 及其 tint/deep 变体
- `color-mix(in oklch, var(--...) 50%, transparent)` 做半透明
- `oklch(from var(--token) l c h / .2)` 做 alpha 调整

### ✅ 色盲可读性自检
- 任何仅靠"绿/红"区分的语义（diff、状态）**必须加第二维度**：
  - diff：背景色 **+** 文字深色 **+** 前缀符号（`+` / `-`）
  - badge：背景色 **+** 文字深色 **+** 字面语义（"运行中"、"等待"）

---

## 3 · 布局与效果护栏

### ❌ 绝不

| 反 pattern | 为什么 |
|---|---|
| 圆角容器 + 左侧强调色竖条 | 典型 AI-slop 套路 |
| 大面积渐变背景（body 满屏渐变、cards 撞色渐变） | AI 工具滥用之一 |
| `scrollIntoView()` | 会破坏 web app 的滚动容器 |
| `<img>` / inline SVG 画插图（卡通 / 吉祥物 / 人像） | 要么用真实素材，要么用 placeholder |
| 装饰性 emoji（🚀 / 🎉 / 💡） | 非品牌元素 · 显得廉价 |
| `text-shadow: 0 0 10px` 的光晕 | 不属于本视觉语言 |
| 多层深阴影（> shadow-2） | 打破手稿"浅油墨"气质 |
| 鼠标 hover 放大组件（transform: scale 1.05） | 惊扰式反馈 · 禁 |

### ✅ 允许 / 推荐

| pattern | 推荐 |
|---|---|
| 小面积**径向**渐变做纸纹理（opacity < .5） | 是 |
| `text-wrap: pretty` | 默认开 |
| CSS Grid / Subgrid | 布局首选 |
| `color-mix()` / `oklch(from ... l c h)` | 做半透明 |
| 内部滚动用 `overflow-y: auto` + 自定义 scrollbar（暗部 mono） | 是 |
| 动画用 `transition` 而非 `animation`（除非明确是 loader） | 是 |
| `prefers-reduced-motion` 兜底 | 必须 |

---

## 4 · 图标护栏

### ❌
- 禁 hand-drawn SVG 角色 / 插图
- 禁从 AI 图像生成器搬图标
- 禁 emoji 代替功能图标

### ✅
- 用 **Phosphor Icons**（`@phosphor-icons/web`）或 **Lucide** 做 UI 图标：功能性、纯线性、与排版节奏匹配
- 真实素材（Orangutan 吉祥物等）用 **placeholder** 直到设计交付

所有图标统一：
```css
.icon {
  width: 18px;
  height: 18px;
  stroke: currentColor;    /* 继承文本色 */
  flex-shrink: 0;
}
.icon--sm { width: 14px; height: 14px; }
.icon--lg { width: 24px; height: 24px; }
```

---

## 5 · 代码文件大小护栏

- **单文件不得超过 1000 行**——超了就拆
- React 场景：单个组件文件 < 400 行；超出拆 subcomponents 到同级目录
- CSS 模块：按组件分开，入口只做 `@import`

---

## 6 · 动画护栏

```css
@media (prefers-reduced-motion: reduce) {
  *,
  *::before,
  *::after {
    animation-duration: 0.01ms !important;
    animation-iteration-count: 1 !important;
    transition-duration: 0.01ms !important;
    scroll-behavior: auto !important;
  }
}
```

- 一般 UI 反馈 transition 时长 **120ms**
- 状态切换 200ms
- 主题切换圆圈掠过 **320ms**（Step H 细化）
- 不用 `spring` 弹跳类缓动
- 不超过 3 个元素同时动画

---

## 7 · 滚动护栏

- 主聊天区：`overflow-y: auto`，不用 `overflow: scroll`
- 需要"滚动到底部"：使用 ref + `element.scrollTop = element.scrollHeight`；**禁用 `scrollIntoView`**（官方指令）
- 无限滚动：顶部 IntersectionObserver；不要轮询

---

## 8 · 可达性护栏

- 所有交互元素 `:focus-visible` 必须有可见轮廓（`box-shadow: 0 0 0 3px oklch(from var(--signal) l c h / .25)`）
- 所有图标按钮必须有 `aria-label`
- 所有表单控件必须有 `<label>`（不能只靠 placeholder）
- 色对比度：
  - 正文 `--content` on `--surface`：≥ 7:1（AAA）
  - 辅助 `--content-muted` on `--surface`：≥ 4.5:1（AA）
  - `--content-soft` 仅用于非关键辅助（< 4.5 但必须 ≥ 3）

---

## 9 · 完工自检表

每个 PR 合入前过一遍：

### 令牌
- [ ] 没有硬编码色值（`#`、`rgb`、`hsl`、裸 `oklch`）出现在组件样式中
- [ ] 所有字号引用 `--text-*` 令牌
- [ ] 所有间距引用 `--space-*` 令牌
- [ ] 所有圆角引用 `--radius-*` 令牌
- [ ] 所有阴影引用 `--shadow-*` 令牌
- [ ] 深色主题切换后视觉仍正确（data-theme="dark" 测过）

### 字体
- [ ] grep 不到 Inter/Roboto/Arial/Fraunces/system-ui/Helvetica
- [ ] 所有衬线文本来自 Newsreader
- [ ] 所有等宽文本来自 Spline Sans Mono
- [ ] `text-wrap: pretty` 在 body 生效

### 布局与视觉
- [ ] 没有圆角 + 左竖条容器
- [ ] 没有大面积渐变背景
- [ ] 没有 `scrollIntoView` 调用
- [ ] 没有装饰性 emoji
- [ ] 没有硬画 SVG 插图（吉祥物 / 角色 / 人像）
- [ ] 点击目标 ≥ 40px（移动 ≥ 44px）
- [ ] 深阴影（blur > 8px）未出现

### 可达性
- [ ] 所有交互元素 `:focus-visible` 有可见轮廓
- [ ] 所有图标按钮有 `aria-label`
- [ ] 所有表单控件有 `<label>`
- [ ] 对比度通过 AA（正文 AAA）
- [ ] `prefers-reduced-motion` 兜底

### 文件大小
- [ ] 没有文件 > 1000 行
- [ ] 单组件文件 < 400 行
- [ ] 没有滥用内联样式（style=""）

### 色盲可读性
- [ ] 仅靠颜色传达的语义都加了第二维度（图标 / 文字 / 前缀）

---

## 10 · 已知陷阱备忘录

记录开发过程中反复踩的坑，方便未来参考。

| 陷阱 | 正确做法 |
|---|---|
| 误把 agent 的 "running" 状态染 Sienna | Moss 是进行中；Sienna 是警告 |
| `--font-body` 忘了 fallback，CDN 失败时 Georgia 不出现 | 永远带完整 fallback 链 |
| 深色主题切换后 `--shadow-1` 太浅看不见 | 深色下 shadow 不透明度加倍（令牌里已处理） |
| `color-mix` 没写 `in oklch` | 会退回到 sRGB 混合，色相漂移 |
| tool block 折叠时 fade 遮挡最后一行 | fade 高度 ≤ 48px；并从 48px 上方开始 |
| diff 行对齐错位（Tab 展开成不同宽度） | 强制 `tab-size: 2`；mono 字体 `font-variant-numeric: tabular-nums` |

---

## 11 · 违规惩罚（给未来维护者的直接信号）

若此次视觉语言在后续迭代中被"便利性"打破（例如有人用 Tailwind 任意色值、有人快速加 emoji 做状态指示、有人换了 Inter 字体）——回退到本提示词作为仲裁唯一真值。

所有偏离都应：
1. 文档化于 `design-decisions.md`（若未来新建）
2. 更新到本文件或 `design-tokens.md`
3. 打上明确的设计评审 commit 标签（`design:v2-manuscript-revision`）

**本视觉语言不拒绝演化，但拒绝无声漂移。**
