# 守卫 · Guardrails

> Step 04 的禁止事项、性能约束、无障碍要求、自检表

---

## 1 · 绝对禁止

| # | 规则 | 原因 |
|---|---|---|
| 1 | 不用 `scrollIntoView` | 全项目铁律，见 Step 02 |
| 2 | 不用胶囊徽章 / pill badge 做表情 | V1 被否决，太像 Slack/Discord |
| 3 | 不用 SVG 笔画做打字指示器 | V2 被否决，不够直观 |
| 4 | 不用 slide-in 做 teammate 入场 | V1 被否决，缺"浮现"感 |
| 5 | 不用骨架屏 / shimmer 做打字指示器 | AI-slop，与手稿气质冲突 |
| 6 | 不在 reactions 容器加背景色 | 必须透明，只有左侧竖线 |
| 7 | 不在 active reaction 加 `--signal-tint` 背景 | 只用文字色 + 斜体 + 下划线 |
| 8 | 不在 hover 时放大 icon（scale） | 只允许背景色变化 |
| 9 | 不在 progress bar 用 `width` 动画 | 触发 layout，用 `left` 位移 |
| 10 | teammate avatar 不用 Sienna hue (42°) | Sienna 专属 lead |

---

## 2 · 性能约束

| 指标 | 上限 | 原因 |
|---|---|---|
| 同时 typing indicator | 3 个 DOM + 1 个 overflow 文本 | 避免长列表 DOM 膨胀 |
| 每条消息 reactions | 8 个不同 emoji | 信息密度上限 |
| 单个 reaction count 显示 | 99（超过显示 99+） | 避免数字过宽 |
| 同时 progress bar | 8 条 | teammate 数量上限 |
| 动画属性 | 只用 transform / opacity / left | 不触发 layout/paint |

---

## 3 · 无障碍清单

| 组件 | 要求 |
|---|---|
| `.typing__indicator` | `aria-label="{name} 正在输入"` |
| `.streaming-cursor` | `aria-hidden="true"` |
| `.msg__react-btn` | `aria-label="添加表情"` |
| `.reaction-picker__item` | `aria-label` 含语义描述（如"点赞"） |
| `.team-card` | `role="button"` + `tabindex="0"` + `aria-current` |
| `.status-dot` | `aria-hidden="true"` |
| status 文本 | 父容器 `aria-live="polite"` |
| picker 焦点 | 打开时移入第一个 item |
| picker 关闭 | Esc 关闭，焦点回到 `msg__react-btn` |

---

## 4 · `prefers-reduced-motion` 兜底

```css
@media (prefers-reduced-motion: reduce) {
  /* 打字指示器 */
  .typing__dot {
    animation: none;
    opacity: 1;
    transform: scale(1);
  }
  .streaming-cursor {
    animation: none;
  }

  /* 入场 / 退场 */
  .team-card--entering,
  .team-card--leaving {
    animation: none;
  }

  /* 状态指示器 */
  .team-card__progress {
    animation: none;
  }
  .status-dot--working,
  .status-dot--waiting {
    animation: none;
    opacity: 1;
  }

  /* Picker */
  .reaction-picker {
    transition: none;
  }
}
```

---

## 5 · 完工自检

### 打字指示器
- [ ] ink dots 三个点，swell/fade 动画流畅
- [ ] streaming cursor 2px 宽，1s step-end 闪烁
- [ ] thinking → streaming 状态切换时 DOM 正确替换
- [ ] 多 agent 打字时各自独立显示
- [ ] >3 个打字时显示 overflow 文本
- [ ] SSE 结束后 cursor DOM 节点已移除
- [ ] `aria-label` 正确设置
- [ ] `prefers-reduced-motion` 下静态显示

### 表情贴图
- [ ] reactions 容器有左侧 2px 竖线
- [ ] 单个 reaction 无背景色、无胶囊
- [ ] active 状态：文字色 + 斜体 + icon 下划线
- [ ] hover 时只变背景色，不放大
- [ ] picker 8 个 emoji，点击外部关闭
- [ ] picker 打开/关闭有 transform + opacity 过渡
- [ ] 添加/移除 reaction 逻辑正确（count 增减、DOM 创建/移除）
- [ ] `msg__react-btn` hover 才显示
- [ ] 无障碍：aria-label、焦点管理

### Teammate 入场
- [ ] pop-in 动画 320ms，scale .92 → 1 + opacity 0 → 1
- [ ] 退场 pop-out 240ms，反向
- [ ] 退场后 DOM 节点已移除
- [ ] status dot 颜色与状态绑定正确
- [ ] progress bar 颜色与状态绑定正确
- [ ] lead 卡片 progress bar 用 `--signal`
- [ ] teammate hue 不包含 42°
- [ ] `prefers-reduced-motion` 下所有动画禁用

### 全局
- [ ] grep 全项目无 `scrollIntoView`
- [ ] 所有颜色引用 token，无硬编码色值
- [ ] 所有过渡使用 token 时长
- [ ] 暗色主题下所有元素可读
