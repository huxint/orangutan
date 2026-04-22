# Orangutan 附件系统 · Step 05 · Citation + Sticky Notes

> 本目录是 **Step 05 · 附件系统** 的最终落地提示词。
> 任何实现 Orangutan 输入附件功能的开发者或 agent，应先阅读本目录，再动手。
> 视觉真值 ground truth：[`web/design/05-attachments/v-merged.html`](../../design/05-attachments/v-merged.html)

---

## 一句话气质

**"学术引用 + 便签贴纸"** —— 添加附件是"插入引用"，附件是贴在手稿底部的便签条，正文内用上标编号关联。

---

## 三条铁律

1. **附件入口是 § 按钮（citation），不是通用 +。** § 是学术引用的符号，点击唤出 citation lookup 浮层；浮层是 autocomplete 风格，不是面板/抽屉/弹窗。
2. **附件展示是便签条（sticky note），不是 chip/pill/tag。** 每张便签有胶带条装饰（`::before`），入场有缩放微旋动画（`sticky-land`），类型用胶带颜色区分。
3. **正文内关联用上标圆圈编号，不用内联 chip。** 上标是 `signal-tint` 底 + `signal-deep` 字的 17px 圆，hover 时加深——像手稿页码的交叉引用。

---

## 文件索引

| 文件 | 用途 |
|---|---|
| [`citation-lookup.md`](./citation-lookup.md) | § 按钮、citation lookup 浮层结构、搜索过滤、键盘导航、分类头、API 绑定 |
| [`sticky-strip.md`](./sticky-strip.md) | 便签条展示区（sticky-strip）、三种便签类型（file/ws/url）、胶带装饰、上标编号关联、动画 |
| [`drag-and-drop.md`](./drag-and-drop.md) | 拖拽上传的墨渍覆盖层（blot-overlay）、文件处理、多文件批量、状态机 |
| [`guardrails.md`](./guardrails.md) | 禁止事项、性能约束、无障碍要求、自检表 |

---

## 依赖与边界

### 依赖
- [`web/prompt/01-visual-language/`](../01-visual-language/) —— 所有 token、字体、颜色、阴影
- [`web/prompt/02-layout/`](../02-layout/) —— send bar 结构（`.send-bar` / `.compose`）、chat-footer 位置
- [`web/prompt/04-chat-dynamics/`](../04-chat-dynamics/) —— status row（附件数可在 status row 中显示）

### 本步骤范围
- § 按钮（替代通用 +）与 citation lookup 浮层
- 便签条（sticky note）展示已添加附件
- 正文内上标编号与便签的关联
- 三种来源：本地上传、工作区文件、URL
- 拖拽上传的墨渍覆盖层
- 工作区文件搜索与选择
- URL 粘贴输入

### 不在本步骤
- 顶栏配置页面 → **Step 06**
- 主题切换圆圈掠过动画 → **Step 07**
- Orangutan 动态 SVG → **Step 08**
- 会话管理 / 记忆管理 / 模型管理 → **Step 06**

---

## 落地顺序建议

1. 读 `citation-lookup.md`，实现 § 按钮 + lookup 浮层 + 键盘导航
2. 读 `sticky-strip.md`，实现便签条展示 + 上标关联 + 动画
3. 读 `drag-and-drop.md`，实现拖拽上传 + 墨渍覆盖层
4. 用 `guardrails.md` 自检表逐条核验
5. 回到 ground truth HTML 做视觉对齐

---

## 视觉真值

唯一的视觉 ground truth 是 `v-merged.html`。当文字描述和它有冲突，以 HTML 为准。

---

## 明确排除（被否决的方案 — 别改回来）

- ❌ **V1 Drop-shelf** —— 标准搁板 UI，缺乏创意
- ❌ **V2 Tab-tray** —— 分页托盘，太像应用级组件
- ❌ **V3 Chip-inline** —— 行内 chip 选择，通用但无特色
- ❌ **V5 Desk Surface** —— 桌面散布纸片，有趣但交互复杂度高
- ❌ **V6 Page Turn** —— 翻页动效好但便签与 compose bar 分层不够清晰（便签展示保留，翻页动效弃用）
- ❌ 通用 `+` 按钮 —— 用 § 替代，体现学术引用气质
- ❌ Chip / pill / tag 展示附件 —— 用便签条 + 胶带装饰
- ❌ Emoji 图标做类型标识 —— 用斜体字母 glyph（`f` / `w` / `u`）
