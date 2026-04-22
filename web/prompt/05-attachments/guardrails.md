# 守卫 · Guardrails

> Step 05 的禁止事项、性能约束、无障碍要求、自检表

---

## 1 · 绝对禁止

| # | 规则 | 原因 |
|---|---|---|
| 1 | 不用 `scrollIntoView` | 全项目铁律 |
| 2 | 不用通用 `+` 按钮做附件入口 | 用 § 符号体现学术引用气质 |
| 3 | 不用 chip / pill / tag 展示附件 | 用便签条 + 胶带装饰 |
| 4 | 不用 emoji / 图标库做类型标识 | 只用斜体字母 glyph（f/w/u） |
| 5 | 不用模态弹窗做 citation lookup | 必须是无模态浮层 |
| 6 | 不用虚线边框闪烁做拖拽提示 | 用墨渍 clip-path 扩散 |
| 7 | 不用 slide-in 做便签入场 | 用 scale + rotate 的 sticky-land |
| 8 | 不在便签上显示删除按钮（非 hover） | × 只在 hover 时显示 |
| 9 | 不在 cite-lookup 分类头用 signal/accent 色 | 永远 content-soft + surface-sunken |
| 10 | 不在便签底色用高饱和度 | `c * .4` 是上限 |

---

## 2 · 性能约束

| 指标 | 上限 | 原因 |
|---|---|---|
| 总附件数 | 9 | 上标编号保持单位数 |
| 单文件大小 | 10 MB | 避免 FormData 超时 |
| 单次拖拽文件数 | 10 | UX 合理上限 |
| citation lookup 结果数 | 10 项（+N more） | 避免长列表 |
| 便签入场动画延迟 | ≤ 200ms（4 × 50ms） | 避免动画感拖沓 |
| 同时 blot-overlay | 1 个 | 防止多次创建 |
| lookup 浮层 DOM | 临时创建，关闭时移除 | 不缓存 |
| blot-overlay DOM | 临时创建，隐藏时移除 | 不缓存 |
| 动画属性 | transform / opacity / clip-path | 不触发 layout |

---

## 3 · 无障碍清单

| 组件 | 要求 |
|---|---|
| `.compose__cite` | `aria-label="插入引用"` + `aria-expanded` |
| `.cite-lookup__field` | `role="combobox"` + `aria-controls` + `aria-activedescendant` |
| `.cite-item` | `role="option"` + `aria-selected` |
| `.cite-lookup` | `role="listbox"` + `aria-label="引用查找"` |
| `.sticky-strip` | `aria-label="已添加引用"` |
| `.sticky__x` | `aria-label="移除引用 N"` |
| `.sup` | `aria-label="引用 N: {filename}"` |
| `.blot-overlay` | `aria-hidden="true"` |
| 添加引用后 | `aria-live="polite"` 播报 `"已添加引用: {name}"` |
| Esc 键 | 关闭 lookup，焦点回 textarea |
| ↑↓ 键 | 在 lookup 结果项间导航 |
| Enter 键 | 选中当前焦点项 |

---

## 4 · `prefers-reduced-motion` 兜底

```css
@media (prefers-reduced-motion: reduce) {
  /* Citation lookup */
  .cite-lookup {
    animation: none;
  }

  /* 便签 */
  .sticky {
    animation: none;
  }

  /* 墨渍 */
  .blot-overlay {
    animation: none;
    clip-path: none;
  }
}
```

---

## 5 · 完工自检

### § 按钮与 Citation Lookup
- [ ] § 按钮用 Newsreader italic 18px
- [ ] 按钮有三态：默认 / hover / active（signal-tint）
- [ ] `aria-expanded` 在 lookup 打开/关闭时正确切换
- [ ] lookup 从 compose bar 下方 6px 处弹出
- [ ] lookup 入场 `cite-rise` 动画 220ms
- [ ] 搜索框自动聚焦
- [ ] ↑↓ 键导航，`is-focused` 高亮
- [ ] Enter 选中，Esc 关闭
- [ ] 分类头 `cite-cat` 用 surface-sunken 底
- [ ] glyph 用 Newsreader italic（f/w/u），focused 时 signal-deep
- [ ] 搜索过滤实时生效，匹配子串 `<strong>` 高亮
- [ ] lookup 宽度与 compose bar 对齐（left:0 right:0）
- [ ] 点击外部关闭 lookup
- [ ] @ 触发 lookup 且不写入 textarea
- [ ] IME 输入不误触 @ 监听

### 便签条
- [ ] sticky-strip 紧贴 compose bar 下方（border-top:none + margin-top:-1px）
- [ ] sticky-strip 背景 surface-sunken
- [ ] 便签有胶带条 ::before（top:-3px, 28×6, radius 1px, opacity .45）
- [ ] 三种类型胶带颜色正确：灰(rule) / 绿(accent) / 赭(signal)
- [ ] 三种类型底色正确且微妙（c * .4）
- [ ] sticky-land 动画 240ms，scale .88 + rotate -2° → 正位
- [ ] 动画错开 50ms（nth-child 递增）
- [ ] × 按钮 hover 才显示
- [ ] × hover 时 signal 色 + signal-tint 底
- [ ] 移除后重新编号
- [ ] 无附件时 strip 不渲染
- [ ] 发送后所有便签清除

### 上标编号
- [ ] 17px 圆形，signal-tint 底，signal-deep 字
- [ ] Newsreader italic bold 10px
- [ ] vertical-align: super
- [ ] hover 时底色加深（l - .04）
- [ ] data-ref 与便签一一对应
- [ ] hover 上标高亮对应便签

### 拖拽
- [ ] 拖拽进入 compose 时 blot-overlay 出现
- [ ] blot-spread 240ms，clip-path circle 12% → 75%
- [ ] compose bar 降至 opacity .25
- [ ] drop 后 blot-shrink 180ms 收缩移除
- [ ] dragCounter 防止重复触发
- [ ] 全局拖拽时 compose bar 边框变 signal 色（is-drop-hint）
- [ ] 单文件 >10 MB 时 toast 提示
- [ ] 拖拽 >10 文件时只取前 10

### 全局
- [ ] grep 全项目无 `scrollIntoView`
- [ ] 所有颜色引用 token，无硬编码色值
- [ ] 所有过渡使用 token 时长
- [ ] 暗色主题下所有元素可读、对比度达标
- [ ] `prefers-reduced-motion` 下所有动画禁用
