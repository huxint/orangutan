#include "primitives.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace orangutan::vkui {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

constexpr std::array<CompanionProfile, 6> kProfiles{{
    {
        .id = CompanionId::oran,
        .key = "oran",
        .name = "Oran",
        .nickname = "小橙",
        .role_title = "主伙伴 · 领队",
        .task_focus = "理解你的目标、把复杂想法拆解为子任务并统筹小队协作",
        .greeting = "嗨！今天想一起推进什么事？直接说，我们来拆分落地。",
        .accent = hex(0xB24A1D),
        .soft = hex(0xF5E3D5),
    },
    {
        .id = CompanionId::scout,
        .key = "scout",
        .name = "Scout",
        .nickname = "阿探",
        .role_title = "调研队员",
        .task_focus = "接手调研、对比方案、摸清上下文与关键线索的子任务",
        .greeting = "需要查资料、翻线索或对比方案？把探索任务分给我吧！",
        .accent = hex(0xC96D28),
        .soft = hex(0xF7E8D6),
    },
    {
        .id = CompanionId::moss,
        .key = "moss",
        .name = "Moss",
        .nickname = "小苔",
        .role_title = "沉淀队员",
        .task_focus = "接手整理纪要、提炼偏好与沉淀长期知识的子任务",
        .greeting = "重要的想法和偏好，我会安静整理成清晰的笔记留下来。",
        .accent = hex(0x3D6B4A),
        .soft = hex(0xE4EFE6),
    },
    {
        .id = CompanionId::fern,
        .key = "fern",
        .name = "Fern",
        .nickname = "小蕨",
        .role_title = "执行队员",
        .task_focus = "接手起草文案、编写改动与推动方案成形的落地子任务",
        .greeting = "拆好步骤就交给我动手实现，干净利落交付给你。",
        .accent = hex(0x476B83),
        .soft = hex(0xE2ECF2),
    },
    {
        .id = CompanionId::pebble,
        .key = "pebble",
        .name = "Pebble",
        .nickname = "石墩",
        .role_title = "把关队员",
        .task_focus = "接手复核细节、检查边界风险与关键操作确认的子任务",
        .greeting = "涉及关键改动或外部动作前，我都会帮你稳稳把关。",
        .accent = hex(0x8C5E36),
        .soft = hex(0xF0E6DA),
    },
    {
        .id = CompanionId::pip,
        .key = "pip",
        .name = "Pip",
        .nickname = "啾啾",
        .role_title = "节奏队员",
        .task_focus = "接手里程碑跟进、时间提醒与收尾闭环的小队子任务",
        .greeting = "啾啾！日程、跟进和轻量提醒都交给我，绝不漏掉重要节点。",
        .accent = hex(0xC08522),
        .soft = hex(0xF7EEDB),
    },
}};

void fill_rgba(float (&dst)[4], Color c) noexcept {
  dst[0] = c.r;
  dst[1] = c.g;
  dst[2] = c.b;
  dst[3] = c.a;
}

}  // namespace

const CompanionProfile& companion_profile(CompanionId id) noexcept {
  const auto idx = static_cast<std::size_t>(id);
  return kProfiles[idx < kProfiles.size() ? idx : 0];
}

void DrawList::clear(float viewport_w, float viewport_h, float dpr) noexcept {
  dpr_ = std::max(dpr, 0.5F);
  root_clip_ = Rect{0.0F, 0.0F, viewport_w * dpr_, viewport_h * dpr_};
  clip_stack_.clear();
  instances_.clear();
}

void DrawList::push_clip(Rect r) {
  Rect px{r.x * dpr_, r.y * dpr_, r.w * dpr_, r.h * dpr_};
  const Rect cur = current_clip_px();
  const float x0 = std::max(cur.x, px.x);
  const float y0 = std::max(cur.y, px.y);
  const float x1 = std::min(cur.x + cur.w, px.x + px.w);
  const float y1 = std::min(cur.y + cur.h, px.y + px.h);
  clip_stack_.push_back(Rect{x0, y0, std::max(0.0F, x1 - x0), std::max(0.0F, y1 - y0)});
}

void DrawList::pop_clip() {
  if (!clip_stack_.empty()) {
    clip_stack_.pop_back();
  }
}

Rect DrawList::current_clip_px() const noexcept {
  return clip_stack_.empty() ? root_clip_ : clip_stack_.back();
}

void DrawList::emit(UiInstance inst) {
  const Rect c = current_clip_px();
  if (c.w <= 0.0F || c.h <= 0.0F) {
    return;
  }
  inst.clip[0] = c.x;
  inst.clip[1] = c.y;
  inst.clip[2] = c.w;
  inst.clip[3] = c.h;
  instances_.push_back(inst);
}

void DrawList::rect(Rect r, float radius, Color fill, Color border, float border_w) {
  rect_radii(r, radius, radius, radius, radius, fill, border, border_w);
}

void DrawList::rect_radii(Rect r, float tl, float tr, float br, float bl, Color fill, Color border, float border_w) {
  if (r.w <= 0.0F || r.h <= 0.0F) {
    return;
  }
  UiInstance inst{};
  inst.rect[0] = r.x * dpr_;
  inst.rect[1] = r.y * dpr_;
  inst.rect[2] = r.w * dpr_;
  inst.rect[3] = r.h * dpr_;
  inst.radii[0] = tl * dpr_;
  inst.radii[1] = tr * dpr_;
  inst.radii[2] = br * dpr_;
  inst.radii[3] = bl * dpr_;
  fill_rgba(inst.color, fill);
  fill_rgba(inst.color2, border);
  inst.params[0] = 0.0F;
  inst.params[1] = border_w * dpr_;
  inst.params[2] = 0.0F;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::rect_gradient(Rect r, float radius, Color top, Color bottom) {
  if (r.w <= 0.0F || r.h <= 0.0F) {
    return;
  }
  UiInstance inst{};
  inst.rect[0] = r.x * dpr_;
  inst.rect[1] = r.y * dpr_;
  inst.rect[2] = r.w * dpr_;
  inst.rect[3] = r.h * dpr_;
  const float rad = radius * dpr_;
  inst.radii[0] = rad;
  inst.radii[1] = rad;
  inst.radii[2] = rad;
  inst.radii[3] = rad;
  fill_rgba(inst.color, top);
  fill_rgba(inst.color2, bottom);
  inst.params[0] = 3.0F;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::shadow(Rect r, float radius, float blur, float offset_y, Color color) {
  if (r.w <= 0.0F || r.h <= 0.0F || blur <= 0.0F) {
    return;
  }
  UiInstance inst{};
  inst.rect[0] = (r.x - blur) * dpr_;
  inst.rect[1] = (r.y + offset_y - blur) * dpr_;
  inst.rect[2] = (r.w + blur * 2.0F) * dpr_;
  inst.rect[3] = (r.h + blur * 2.0F) * dpr_;
  const float rad = radius * dpr_;
  inst.radii[0] = rad;
  inst.radii[1] = rad;
  inst.radii[2] = rad;
  inst.radii[3] = rad;
  fill_rgba(inst.color, color);
  inst.params[0] = 2.0F;
  inst.params[2] = blur * dpr_;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::circle(float cx, float cy, float radius, Color fill, Color border, float border_w) {
  if (radius <= 0.0F) {
    return;
  }
  rect(Rect{cx - radius, cy - radius, radius * 2.0F, radius * 2.0F}, radius, fill, border, border_w);
}

void DrawList::ellipse(float cx, float cy, float rx, float ry, Color fill, Color border, float border_w) {
  if (rx <= 0.0F || ry <= 0.0F) {
    return;
  }
  rect(Rect{cx - rx, cy - ry, rx * 2.0F, ry * 2.0F}, std::min(rx, ry), fill, border, border_w);
}

void DrawList::triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color fill, float round_r) {
  const float pad = round_r + 2.0F;
  const float min_x = std::min({x0, x1, x2}) - pad;
  const float min_y = std::min({y0, y1, y2}) - pad;
  const float max_x = std::max({x0, x1, x2}) + pad;
  const float max_y = std::max({y0, y1, y2}) + pad;
  const float w = std::max(max_x - min_x, 1.0F);
  const float h = std::max(max_y - min_y, 1.0F);

  UiInstance inst{};
  inst.rect[0] = min_x * dpr_;
  inst.rect[1] = min_y * dpr_;
  inst.rect[2] = w * dpr_;
  inst.rect[3] = h * dpr_;
  inst.radii[0] = (x0 - min_x) / w;
  inst.radii[1] = (y0 - min_y) / h;
  inst.radii[2] = (x1 - min_x) / w;
  inst.radii[3] = (y1 - min_y) / h;
  fill_rgba(inst.color, fill);
  inst.color2[0] = (x2 - min_x) / w;
  inst.color2[1] = (y2 - min_y) / h;
  inst.params[0] = 4.0F;
  inst.params[2] = round_r * dpr_;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::line(float x0, float y0, float x1, float y1, float thickness, Color color) {
  const float pad = thickness + 2.0F;
  const float min_x = std::min(x0, x1) - pad;
  const float min_y = std::min(y0, y1) - pad;
  const float max_x = std::max(x0, x1) + pad;
  const float max_y = std::max(y0, y1) + pad;
  const float w = std::max(max_x - min_x, 1.0F);
  const float h = std::max(max_y - min_y, 1.0F);

  UiInstance inst{};
  inst.rect[0] = min_x * dpr_;
  inst.rect[1] = min_y * dpr_;
  inst.rect[2] = w * dpr_;
  inst.rect[3] = h * dpr_;
  inst.radii[0] = (x0 - min_x) / w;
  inst.radii[1] = (y0 - min_y) / h;
  inst.radii[2] = (x1 - min_x) / w;
  inst.radii[3] = (y1 - min_y) / h;
  fill_rgba(inst.color, color);
  inst.params[0] = 5.0F;
  inst.params[1] = thickness * dpr_;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::arc(float cx, float cy, float radius, float start_rad, float sweep_rad, float thickness, Color color) {
  const float pad = thickness + 2.0F;
  const float half = radius + pad;
  UiInstance inst{};
  inst.rect[0] = (cx - half) * dpr_;
  inst.rect[1] = (cy - half) * dpr_;
  inst.rect[2] = (half * 2.0F) * dpr_;
  inst.rect[3] = (half * 2.0F) * dpr_;
  fill_rgba(inst.color, color);
  inst.uv[0] = start_rad;
  inst.uv[1] = sweep_rad;
  inst.params[0] = 6.0F;
  inst.params[1] = thickness * dpr_;
  inst.params[2] = radius * dpr_;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::glyph(Rect r, float u0, float v0, float u1, float v1, Color color, float gamma) {
  if (r.w <= 0.0F || r.h <= 0.0F) {
    return;
  }
  UiInstance inst{};
  inst.rect[0] = r.x * dpr_;
  inst.rect[1] = r.y * dpr_;
  inst.rect[2] = r.w * dpr_;
  inst.rect[3] = r.h * dpr_;
  fill_rgba(inst.color, color);
  inst.uv[0] = u0;
  inst.uv[1] = v0;
  inst.uv[2] = u1;
  inst.uv[3] = v1;
  inst.params[0] = 1.0F;
  inst.params[2] = gamma;
  inst.params[3] = 1.0F;
  emit(inst);
}

void DrawList::icon(IconKind kind, float cx, float cy, float size, Color color) {
  const float r = size * 0.5F;
  const float s = std::max(1.5F, size * 0.105F);
  switch (kind) {
    case IconKind::chat:
      rect(Rect{cx - r * 0.85F, cy - r * 0.72F, r * 1.70F, r * 1.28F}, r * 0.36F, Color{0, 0, 0, 0}, color, s);
      triangle(cx - r * 0.45F, cy + r * 0.50F, cx - r * 0.10F, cy + r * 0.50F, cx - r * 0.55F, cy + r * 0.88F, color);
      break;
    case IconKind::memory:
      rect(Rect{cx - r * 0.72F, cy - r * 0.82F, r * 1.44F, r * 1.64F}, r * 0.24F, Color{0, 0, 0, 0}, color, s);
      line(cx - r * 0.38F, cy - r * 0.32F, cx + r * 0.38F, cy - r * 0.32F, s, color);
      line(cx - r * 0.38F, cy + r * 0.05F, cx + r * 0.38F, cy + r * 0.05F, s, color);
      line(cx - r * 0.38F, cy + r * 0.42F, cx + r * 0.12F, cy + r * 0.42F, s, color);
      break;
    case IconKind::bell:
      arc(cx, cy - r * 0.05F, r * 0.58F, kPi, kPi, s, color);
      line(cx - r * 0.58F, cy - r * 0.05F, cx - r * 0.72F, cy + r * 0.45F, s, color);
      line(cx + r * 0.58F, cy - r * 0.05F, cx + r * 0.72F, cy + r * 0.45F, s, color);
      line(cx - r * 0.80F, cy + r * 0.45F, cx + r * 0.80F, cy + r * 0.45F, s, color);
      circle(cx, cy + r * 0.74F, s * 0.9F, color);
      break;
    case IconKind::sparkles:
      triangle(cx, cy - r * 0.90F, cx + r * 0.24F, cy, cx - r * 0.24F, cy, color, 0.5F);
      triangle(cx, cy + r * 0.90F, cx - r * 0.24F, cy, cx + r * 0.24F, cy, color, 0.5F);
      triangle(cx - r * 0.90F, cy, cx, cy - r * 0.24F, cx, cy + r * 0.24F, color, 0.5F);
      triangle(cx + r * 0.90F, cy, cx, cy + r * 0.24F, cx, cy - r * 0.24F, color, 0.5F);
      break;
    case IconKind::settings:
      circle(cx, cy, r * 0.46F, Color{0, 0, 0, 0}, color, s);
      for (int i = 0; i < 6; ++i) {
        const float a = static_cast<float>(i) * (kPi / 3.0F);
        line(cx + std::cos(a) * r * 0.56F, cy + std::sin(a) * r * 0.56F,
             cx + std::cos(a) * r * 0.82F, cy + std::sin(a) * r * 0.82F, s * 1.1F, color);
      }
      break;
    case IconKind::plus:
      line(cx - r * 0.65F, cy, cx + r * 0.65F, cy, s * 1.15F, color);
      line(cx, cy - r * 0.65F, cx, cy + r * 0.65F, s * 1.15F, color);
      break;
    case IconKind::send:
      line(cx, cy + r * 0.65F, cx, cy - r * 0.58F, s * 1.25F, color);
      line(cx - r * 0.48F, cy - r * 0.12F, cx, cy - r * 0.62F, s * 1.25F, color);
      line(cx + r * 0.48F, cy - r * 0.12F, cx, cy - r * 0.62F, s * 1.25F, color);
      break;
    case IconKind::stop:
      rect(Rect{cx - r * 0.46F, cy - r * 0.46F, r * 0.92F, r * 0.92F}, r * 0.18F, color);
      break;
    case IconKind::sun:
      circle(cx, cy, r * 0.38F, Color{0, 0, 0, 0}, color, s);
      for (int i = 0; i < 8; ++i) {
        const float a = static_cast<float>(i) * (kPi / 4.0F);
        line(cx + std::cos(a) * r * 0.58F, cy + std::sin(a) * r * 0.58F,
             cx + std::cos(a) * r * 0.82F, cy + std::sin(a) * r * 0.82F, s, color);
      }
      break;
    case IconKind::moon:
      arc(cx + r * 0.08F, cy, r * 0.64F, 0.55F, kPi * 1.38F, s * 1.15F, color);
      break;
    case IconKind::check:
      line(cx - r * 0.55F, cy + r * 0.04F, cx - r * 0.12F, cy + r * 0.48F, s * 1.25F, color);
      line(cx - r * 0.12F, cy + r * 0.48F, cx + r * 0.62F, cy - r * 0.42F, s * 1.25F, color);
      break;
    case IconKind::trash:
      line(cx - r * 0.65F, cy - r * 0.48F, cx + r * 0.65F, cy - r * 0.48F, s, color);
      rect(Rect{cx - r * 0.48F, cy - r * 0.42F, r * 0.96F, r * 1.12F}, r * 0.16F, Color{0, 0, 0, 0}, color, s);
      line(cx - r * 0.22F, cy - r * 0.72F, cx + r * 0.22F, cy - r * 0.72F, s, color);
      break;
    case IconKind::shield:
      rect(Rect{cx - r * 0.62F, cy - r * 0.72F, r * 1.24F, r * 1.04F}, r * 0.22F, Color{0, 0, 0, 0}, color, s);
      line(cx - r * 0.58F, cy + r * 0.26F, cx, cy + r * 0.80F, s, color);
      line(cx + r * 0.58F, cy + r * 0.26F, cx, cy + r * 0.80F, s, color);
      break;
    case IconKind::split_tasks:
      circle(cx - r * 0.55F, cy, r * 0.22F, color);
      circle(cx + r * 0.55F, cy - r * 0.48F, r * 0.20F, color);
      circle(cx + r * 0.55F, cy + r * 0.48F, r * 0.20F, color);
      line(cx - r * 0.32F, cy, cx + r * 0.34F, cy - r * 0.42F, s, color);
      line(cx - r * 0.32F, cy, cx + r * 0.34F, cy + r * 0.42F, s, color);
      break;
    case IconKind::folder:
      rect(Rect{cx - r * 0.78F, cy - r * 0.48F, r * 1.56F, r * 1.12F}, r * 0.20F, Color{0, 0, 0, 0}, color, s);
      line(cx - r * 0.72F, cy - r * 0.62F, cx - r * 0.12F, cy - r * 0.62F, s, color);
      break;
  }
}

namespace {

struct Pose3D {
  float yaw{0.0F};     // true 3D horizontal head turn (radians)
  float tilt{0.0F};    // true 3D playful head tilt (radians)
  float hop_y{0.0F};   // true 3D vertical hop height in world units
  float blink{0.0F};   // 0=open, 1=closed 3D eyelids
  float breath{0.0F};  // gentle chest breathing wave [-1, 1]
  float wag{0.0F};     // extra secondary tail/wing energy
};

// Pure-code autonomous 3D companion life engine ("有时可以会动"):
// calm breathing + periodic realistic 3D blinking + staggered occasional
// 3D head-turns, playful head-tilts, and joyful hops + subtle cursor look-at!
[[nodiscard]] Pose3D compute_pose_3d(CompanionId id, CompanionMood mood, float time_sec, bool animate,
                                     float pet_bounce, float hover_amount, float look_dx) noexcept {
  Pose3D pose{};
  const float idx_f = static_cast<float>(id);
  const float phase = animate ? time_sec : 0.0F;

  // Natural subtle 3/4 sculptural hero stance so every 3D figure shows rich depth immediately
  pose.yaw = ((static_cast<int>(id) % 2 == 0) ? 0.14F : -0.14F);

  if (animate) {
    // 1. Calm continuous breathing
    pose.breath = std::sin(phase * 1.9F + idx_f * 1.17F);

    // 2. Realistic periodic 3D eyelid blink (~every 4.1-4.9s, 180ms smooth close-open)
    const float blink_period = 4.1F + std::fmod(idx_f * 0.41F, 1.1F);
    const float bt = std::fmod(phase + idx_f * 1.37F, blink_period);
    if (bt > blink_period - 0.18F) {
      const float u = (bt - (blink_period - 0.18F)) / 0.18F;
      pose.blink = std::sin(u * kPi);
    }

    // 3. Occasional autonomous 3D micro-actions every ~4.6-6.2 seconds per companion
    const float period = 4.8F + std::fmod(idx_f * 0.67F, 1.5F);
    const float t_mod = std::fmod(phase + idx_f * 1.95F, period);
    const int cycle = static_cast<int>((phase + idx_f * 1.95F) / period) + static_cast<int>(id) * 5;
    const int action = cycle % 3;  // 0: 3D curious look around, 1: playful 3D tilt, 2: joyful 3D hop

    if (action == 0 && t_mod < 1.55F) {
      const float u = t_mod / 1.55F;
      const float env = std::sin(u * kPi);
      const float dir = ((cycle / 3) % 2 == 0) ? 1.0F : -1.0F;
      pose.yaw += dir * 0.34F * env;
      pose.tilt += dir * 0.08F * env;
    } else if (action == 1 && t_mod < 1.30F) {
      const float u = t_mod / 1.30F;
      const float env = std::sin(u * kPi);
      const float dir = ((cycle / 3) % 2 == 0) ? 1.0F : -1.0F;
      pose.tilt += dir * 0.17F * env * env;
      pose.yaw -= dir * 0.12F * env;
    } else if (action == 2 && t_mod < 0.85F) {
      const float u = t_mod / 0.85F;
      if (u < 0.18F) {
        pose.breath -= 2.2F * std::sin((u / 0.18F) * kPi);  // anticipation squash
      } else if (u < 0.78F) {
        const float air = (u - 0.18F) / 0.60F;
        pose.hop_y += 0.22F * std::sin(air * kPi);
        pose.tilt += 0.08F * std::sin(air * kPi * 2.0F);
        pose.wag += 2.0F * std::sin(air * kPi);
      } else {
        const float land = (u - 0.78F) / 0.22F;
        pose.breath -= 1.8F * std::sin(land * kPi);  // bouncy landing squash
      }
    }

    // 4. Mood-driven liveliness
    if (mood == CompanionMood::working || mood == CompanionMood::thinking) {
      pose.yaw += 0.18F * std::sin(phase * 3.1F + idx_f);
      pose.tilt += 0.09F * std::sin(phase * 4.4F);
      pose.wag += 1.2F;
    } else if (mood == CompanionMood::celebrating) {
      pose.hop_y += 0.14F * std::abs(std::sin(phase * 5.0F));
      pose.tilt += 0.12F * std::sin(phase * 6.0F);
    }
  }

  // 5. Real-time 3D cursor gaze tracking + click-to-pet happy jump!
  pose.yaw += std::clamp(look_dx, -1.0F, 1.0F) * 0.22F;
  if (hover_amount > 0.0F) {
    pose.tilt += 0.09F * hover_amount;
    pose.wag += 1.5F * hover_amount;
  }
  if (pet_bounce > 0.0F) {
    const float s = std::sin(pet_bounce * kPi);
    pose.hop_y += 0.28F * s;
    pose.tilt += 0.19F * std::sin(pet_bounce * kPi * 2.0F);
    pose.blink = std::max(pose.blink, s * 0.85F);  // happy squint!
    pose.wag += 3.0F * s;
  }

  return pose;
}

}  // namespace

void DrawList::companion(CompanionId id, float cx, float cy, float r, CompanionMood mood, float time_sec,
                         bool animate, float pet_bounce, float hover_amount) {
  const auto idx = static_cast<std::size_t>(id);
  const float look_dx = (cursor_x_ >= 0.0F) ? std::clamp((cursor_x_ - cx) / 280.0F, -1.0F, 1.0F) : 0.0F;
  const auto pose = compute_pose_3d(id, mood, time_sec, animate, pet_bounce, hover_amount, look_dx);

  // Pure-code backgroundless 3D raymarched PBR figurine (kind == 7):
  // zero PNG textures, zero circular stickers, real raymarched 3D shadows & rim light!
  const float scale = 1.0F + hover_amount * 0.05F;
  const float half = r * 1.28F * scale;

  UiInstance inst{};
  inst.rect[0] = (cx - half) * dpr_;
  inst.rect[1] = (cy - half) * dpr_;
  inst.rect[2] = (half * 2.0F) * dpr_;
  inst.rect[3] = (half * 2.0F) * dpr_;

  inst.radii[0] = pose.yaw;
  inst.radii[1] = pose.tilt;
  inst.radii[2] = pose.hop_y;
  inst.radii[3] = pose.blink;

  inst.color[0] = static_cast<float>(idx < 6U ? idx : 0U);
  inst.color[1] = animate ? time_sec : 0.0F;
  inst.color[2] = static_cast<float>(mood);
  inst.color[3] = 1.0F;

  inst.color2[0] = pose.breath;
  inst.color2[1] = pose.wag;
  inst.color2[2] = 0.0F;
  inst.color2[3] = 0.0F;

  inst.params[0] = 7.0F;
  inst.params[1] = hover_amount + (pet_bounce > 0.0F ? 0.65F : 0.0F);
  inst.params[2] = dark_surface_ ? 1.0F : 0.0F;
  inst.params[3] = 1.0F;
  emit(inst);
}

}  // namespace orangutan::vkui
