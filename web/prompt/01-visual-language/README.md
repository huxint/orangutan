# Orangutan 视觉语言 · V2 · Manuscript（手稿）

> 本目录是 **Step 01 · 视觉语言** 的最终落地提示词。
> 任何实现 Orangutan 前端的开发者或 agent，应先阅读本目录，再动手。
> 视觉真值 ground truth：[`web/design/01-visual-language/v2-manuscript.html`](../../design/01-visual-language/v2-manuscript.html)

---

## 一句话气质

**"学者工作桌" · 单衬线家族 · 暖大麻纸 · 墨蓝字 · 赭红主信号 · 软圆角 · 浅油墨印痕**

比报纸更近一步 —— 不再是铅印社论的锐利，而是一张被翻动过无数次、写满批注的旧稿子。

---

## 三条铁律（贯穿整个项目）

1. **字族单一：**所有衬线文本都来自 **Newsreader**（display/body/pull-quote 都用它，靠 weight + italic + opsz 做层级）；所有等宽都来自 **Spline Sans Mono**。不引入第三种字体。
2. **赭红 Sienna 是主信号色 · 墨绿 Moss 是次信号色。**对换自 V1，体现"学者批注"的气质——红笔批改多于绿色圈注。
3. **软圆角（4/8/12）+ 浅油墨阴影**，永远不要 sharp-0 的锐角；也永远不要重投影。

---

## 文件索引

| 文件 | 用途 |
|---|---|
| [`design-tokens.md`](./design-tokens.md) | 所有设计令牌（色 / 字 / 空间 / 圆角 / 阴影）的 CSS 变量定义 + 命名语义 + 深色主题映射 |
| [`typography.md`](./typography.md) | 字体加载、完整字号/字重/行高/字距阶梯、应用规则 |
| [`components.md`](./components.md) | 每个 UI 原子（按钮、徽章、输入、消息块、工具块、diff、math、reply-quote、image、avatar、thinking）的具体规格 |
| [`guardrails.md`](./guardrails.md) | 必须遵守的"不要这样做"清单、品牌反 AI-slop 规则、完工自检表 |

---

## 气质锚点（写任何新组件前默念）

- **看起来像一张手稿。** 纸比墨温，字比纸冷。
- **不是一堆气泡。** 消息靠虚线分隔、头部小字署名，正文像段落而不是对话框。
- **信号色稀缺。** 一屏上不超过 3-4 处 Sienna，不超过 2-3 处 Moss。
- **阴影是湿墨的洇开，不是物体的立体。** 永远 < 2px 模糊半径，0-2px 的偏移，5-7% 不透明度。
- **等宽字在衬线正文里，不刺眼。** Spline Sans Mono 的圆润字形比 JetBrains / Geist Mono 更"同家族"。

---

## 明确排除（选 V2 时被否决的方案 — 别改回来）

- ❌ 锐角 0px 圆角（V1 的气质）
- ❌ Sans-serif 正文（V3 的气质）— V2 的正文必须是衬线
- ❌ 冷灰蓝底（V3 的气质）— V2 是暖色纸
- ❌ 黑白 + 单一信号色（V1 的节制）— V2 两种信号色并存
- ❌ 三字家族（V1 的 Instrument + Newsreader + Plex）— V2 单家族纪律必须守住
- ❌ Moss 做主色 —— Moss 是次信号。违反 = 整个气质失色

---

## 落地时的顺序

1. 把 `design-tokens.md` 里的 CSS 变量完整抄到项目的全局样式文件（如 `src/styles/tokens.css`）
2. 根据 `typography.md` 注入 Google Fonts `<link>` 并配好 `font-feature-settings`
3. 按 `components.md` 实现每个原子，只允许引用 token，**禁止在组件样式里写具体色值 / 字号**
4. 实现完用 `guardrails.md` 的自检表逐条核验

---

## 视觉真值

唯一的视觉 ground truth 是 `v2-manuscript.html`。当文字描述和它有冲突，以 HTML 为准。
