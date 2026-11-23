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

struct FigurinePose {
  float lift_norm{0.0F};  // vertical hop offset as fraction of radius
  float tilt_rad{0.0F};   // playful organic head/body tilt
  float squash_x{1.0F};   // horizontal squash/stretch around feet
  float stretch_y{1.0F};  // vertical breathing/hop stretch around feet
  float sway_x{0.0F};     // subtle curious upper-body sway
};

// Computes natural, occasional autonomous movement ("有时会动") per companion:
// calm slow breathing most of the time, punctuated every ~4.5-6.5s by an organic
// head tilt, curious sway, or light joyful hop (staggered uniquely per companion).
[[nodiscard]] FigurinePose compute_figurine_pose(CompanionId id, CompanionMood mood, float time_sec, bool animate,
                                                 float pet_bounce, float hover_amount) noexcept {
  FigurinePose pose{};
  if (!animate && pet_bounce <= 0.0F && hover_amount <= 0.0F) {
    return pose;
  }

  const float idx_f = static_cast<float>(id);
  const float phase = animate ? time_sec : 0.0F;

  // 1. Calm organic breathing (barely perceptible chest rise, feet stay planted)
  if (animate) {
    const float breath = std::sin(phase * 1.85F + idx_f * 1.17F);
    pose.stretch_y += breath * 0.018F;
    pose.squash_x -= breath * 0.010F;
  }

  // 2. Occasional autonomous micro-behaviors every ~4.8-6.4 seconds per companion
  if (animate) {
    const float period = 5.2F + std::fmod(idx_f * 0.73F, 1.9F);
    const float t_mod = std::fmod(phase + idx_f * 2.13F, period);
    const int cycle_id = static_cast<int>((phase + idx_f * 2.13F) / period) + static_cast<int>(id) * 7;
    const int action = cycle_id % 3;  // 0: head tilt, 1: playful hop, 2: curious sway

    if (action == 0 && t_mod < 1.35F) {
      // Smooth playful head tilt & return
      const float env = std::sin((t_mod / 1.35F) * kPi);
      const float dir = ((cycle_id / 3) % 2 == 0) ? 1.0F : -1.0F;
      pose.tilt_rad += dir * 0.12F * env * env;
      pose.sway_x += dir * 0.022F * env;
    } else if (action == 1 && t_mod < 0.85F) {
      // Light joyful hop with anticipation squash & bouncy landing
      const float u = t_mod / 0.85F;
      if (u < 0.18F) {
        const float sq = std::sin((u / 0.18F) * kPi);
        pose.squash_x += 0.06F * sq;
        pose.stretch_y -= 0.06F * sq;
      } else if (u < 0.76F) {
        const float air = (u - 0.18F) / 0.58F;
        const float arc_h = std::sin(air * kPi);
        pose.lift_norm += 0.20F * arc_h;
        pose.stretch_y += 0.055F * arc_h;
        pose.squash_x -= 0.035F * arc_h;
        pose.tilt_rad += 0.055F * std::sin(air * kPi * 2.0F);
      } else {
        const float land = (u - 0.76F) / 0.24F;
        const float sq = std::sin(land * kPi);
        pose.squash_x += 0.055F * sq;
        pose.stretch_y -= 0.050F * sq;
      }
    } else if (action == 2 && t_mod < 1.50F) {
      // Curious double look/sway
      const float u = t_mod / 1.50F;
      const float env = std::sin(u * kPi);
      const float wave = std::sin(u * kPi * 2.0F);
      pose.sway_x += 0.042F * wave * env;
      pose.tilt_rad += 0.075F * wave * env;
    }
  }

  // 3. Mood-driven expression (active collaboration / guarding / celebrating)
  if (animate && (mood == CompanionMood::working || mood == CompanionMood::thinking)) {
    const float w = std::sin(phase * 4.8F + idx_f);
    pose.tilt_rad += 0.07F * w;
    pose.lift_norm += std::max(0.0F, std::sin(phase * 5.6F)) * 0.07F;
  } else if (animate && mood == CompanionMood::celebrating) {
    const float hop = std::abs(std::sin(phase * 5.2F));
    pose.lift_norm += 0.14F * hop;
    pose.tilt_rad += 0.09F * std::sin(phase * 6.5F);
  }

  // 4. Hover attentive perk-up & click-to-pet joyful spring bounce
  if (hover_amount > 0.0F) {
    pose.tilt_rad += 0.065F * hover_amount;
    pose.stretch_y += 0.030F * hover_amount;
    pose.squash_x += 0.020F * hover_amount;
  }
  if (pet_bounce > 0.0F) {
    const float s = std::sin(pet_bounce * kPi);
    pose.lift_norm += 0.26F * s;
    pose.tilt_rad += 0.16F * std::sin(pet_bounce * kPi * 2.0F);
    pose.stretch_y += 0.08F * s;
    pose.squash_x -= 0.04F * s;
  }

  return pose;
}

}  // namespace

void DrawList::companion(CompanionId id, float cx, float cy, float r, CompanionMood mood, float time_sec,
                         bool animate, float pet_bounce, float hover_amount) {
  const auto& prof = companion_profile(id);
  const auto pose = compute_figurine_pose(id, mood, time_sec, animate, pet_bounce, hover_amount);
  const float phase = animate ? time_sec : 0.0F;
  const float lift_px = pose.lift_norm * r;
  const float y = cy - lift_px;

  if (textured_companions_) {
    const auto idx = static_cast<std::size_t>(id);
    if (idx < 6U) {
      // Pure backgroundless freestanding 3D figurine — NO circle or box backdrop!
      // Size generous (2.25 * r) so the transparent cutout silhouette matches optical visual weight.
      const float scale = 1.0F + hover_amount * 0.055F;
      const float half = r * 1.16F * scale;

      // Grounded 3D floor contact shadow directly under the figurine's base;
      // physically shrinks and fades as the figurine hops upward!
      if (r >= 15.0F) {
        const float air_factor = std::clamp(1.0F - pose.lift_norm * 1.8F, 0.35F, 1.0F);
        const float sw = half * 1.24F * air_factor;
        const float sh = std::max(4.0F, half * 0.22F * air_factor);
        const float sy = cy + half * 0.78F - sh * 0.5F;
        const float shadow_alpha = (dark_surface_ ? 0.42F : 0.22F) * air_factor;
        shadow(Rect{cx - sw * 0.5F, sy, sw, sh}, sh * 0.5F, std::max(5.0F, r * 0.22F), 0.0F,
               hex(0x1A130D, shadow_alpha));
      }

      // Subtle glowing halo ONLY beneath the floor when actively working/guarding (never a solid backdrop!)
      if (mood == CompanionMood::working || mood == CompanionMood::thinking) {
        const float pulse = 0.55F + 0.45F * std::sin(phase * 3.5F);
        const float sw = half * 1.45F;
        const float sh = std::max(4.0F, half * 0.22F);
        shadow(Rect{cx - sw * 0.5F, cy + half * 0.76F - sh * 0.5F, sw, sh}, sh * 0.5F, std::max(6.0F, r * 0.28F),
               0.0F, prof.accent.with_alpha(0.28F * pulse));
      } else if (mood == CompanionMood::guarding) {
        const float sw = half * 1.50F;
        const float sh = std::max(4.0F, half * 0.24F);
        shadow(Rect{cx - sw * 0.5F, cy + half * 0.76F - sh * 0.5F, sw, sh}, sh * 0.5F, std::max(7.0F, r * 0.30F),
               0.0F, hex(0xD48B22, 0.34F));
      }

      const float col = static_cast<float>(idx % 3U);
      const float row = static_cast<float>(idx / 3U);
      const float tile_w = 1.0F / 3.0F;
      const float tile_h = 1.0F / 2.0F;
      constexpr float kBleed = 0.004F;

      UiInstance inst{};
      inst.rect[0] = (cx - half) * dpr_;
      inst.rect[1] = (y - half) * dpr_;
      inst.rect[2] = (half * 2.0F) * dpr_;
      inst.rect[3] = (half * 2.0F) * dpr_;
      inst.radii[0] = pose.tilt_rad;
      inst.radii[1] = pose.squash_x;
      inst.radii[2] = pose.stretch_y;
      inst.radii[3] = pose.sway_x;
      fill_rgba(inst.color, Color{1, 1, 1, 1});
      inst.uv[0] = (col + kBleed) * tile_w;
      inst.uv[1] = (row + kBleed) * tile_h;
      inst.uv[2] = (col + (1.0F - kBleed)) * tile_w;
      inst.uv[3] = (row + (1.0F - kBleed)) * tile_h;
      inst.params[0] = 7.0F;
      inst.params[1] = hover_amount + (pet_bounce > 0.0F ? 0.65F : 0.0F);
      inst.params[2] = dark_surface_ ? 0.90F : 0.25F;
      inst.params[3] = 1.0F;
      emit(inst);
      return;
    }
  }

  // Periodic natural blink (~every 4.2s)
  const float blink_cycle = std::fmod(phase + static_cast<float>(id) * 0.9F, 4.4F);
  const bool blinking = animate && blink_cycle > 4.24F;
  const bool happy_eyes = mood == CompanionMood::celebrating || pet_bounce > 0.05F;

  // Soft warm ambient medallion halo behind each companion
  circle(cx, cy + r * 0.04F, r * 1.02F, prof.soft.with_alpha(0.92F), prof.accent.with_alpha(0.24F),
         std::max(1.0F, r * 0.045F));

  // Subtle thinking/working orbiting accent ring
  if (mood == CompanionMood::thinking || mood == CompanionMood::working) {
    const float spin = phase * 2.6F;
    arc(cx, cy + r * 0.04F, r * 1.07F, spin, kPi * 0.65F, std::max(1.8F, r * 0.065F), prof.accent);
  } else if (mood == CompanionMood::guarding) {
    circle(cx, cy + r * 0.04F, r * 1.08F, Color{0, 0, 0, 0}, hex(0xB37620, 0.72F), std::max(1.6F, r * 0.055F));
  }

  const Color ink = hex(0x2B2118);
  const Color white = hex(0xFFFDF8);
  const Color cheek = hex(0xEE8E72, 0.52F);

  auto draw_eye = [&](float ex, float ey, float er) {
    if (happy_eyes) {
      arc(ex, ey + er * 0.25F, er * 1.15F, kPi * 1.12F, kPi * 0.76F, std::max(1.6F, r * 0.065F), ink);
    } else if (blinking) {
      line(ex - er, ey, ex + er, ey, std::max(1.5F, r * 0.06F), ink);
    } else {
      circle(ex, ey, er, ink);
      circle(ex - er * 0.30F, ey - er * 0.32F, er * 0.34F, white);
    }
  };

  switch (id) {
    case CompanionId::oran: {
      // Cute terracotta orangutan with cream muzzle & lively green sprout leaf on head!
      const Color fur = hex(0xC65A2B);
      const Color fur_hi = hex(0xD87242);
      const Color muzzle = hex(0xF8E3C6);
      const Color leaf = hex(0x4F8B5B);
      const Color leaf_hi = hex(0x6BA975);
      const float sway = animate ? std::sin(phase * 3.1F) * (r * 0.06F) : 0.0F;

      // Sprout stem + twin playful leaves
      line(cx, y - r * 0.62F, cx + sway * 0.6F, y - r * 0.94F, std::max(1.6F, r * 0.065F), leaf);
      ellipse(cx - r * 0.17F + sway, y - r * 0.92F, r * 0.19F, r * 0.11F, leaf);
      ellipse(cx + r * 0.18F + sway, y - r * 0.96F, r * 0.17F, r * 0.10F, leaf_hi);

      // Round ears
      circle(cx - r * 0.62F, y - r * 0.04F, r * 0.22F, fur);
      circle(cx - r * 0.62F, y - r * 0.04F, r * 0.12F, muzzle);
      circle(cx + r * 0.62F, y - r * 0.04F, r * 0.22F, fur);
      circle(cx + r * 0.62F, y - r * 0.04F, r * 0.12F, muzzle);

      // Head & warm forehead highlight
      ellipse(cx, y + r * 0.03F, r * 0.64F, r * 0.60F, fur);
      ellipse(cx, y - r * 0.24F, r * 0.42F, r * 0.22F, fur_hi.with_alpha(0.55F));

      // Warm cream facial mask & snout
      ellipse(cx - r * 0.18F, y - r * 0.04F, r * 0.25F, r * 0.23F, muzzle);
      ellipse(cx + r * 0.18F, y - r * 0.04F, r * 0.25F, r * 0.23F, muzzle);
      ellipse(cx, y + r * 0.22F, r * 0.46F, r * 0.34F, muzzle);

      // Rosy cheeks
      ellipse(cx - r * 0.34F, y + r * 0.16F, r * 0.12F, r * 0.075F, cheek);
      ellipse(cx + r * 0.34F, y + r * 0.16F, r * 0.12F, r * 0.075F, cheek);

      // Expressive eyes + gentle smile
      draw_eye(cx - r * 0.20F, y - r * 0.04F, r * 0.095F);
      draw_eye(cx + r * 0.20F, y - r * 0.04F, r * 0.095F);
      ellipse(cx, y + r * 0.13F, r * 0.075F, r * 0.050F, hex(0x6E3B23));
      arc(cx, y + r * 0.18F, r * 0.17F, kPi * 0.18F, kPi * 0.64F, std::max(1.5F, r * 0.055F), ink);
      break;
    }

    case CompanionId::scout: {
      // Curious golden-amber forest fox
      const Color fur = hex(0xD97732);
      const Color tip = hex(0x422B1E);
      const Color cream = hex(0xFAF0E1);

      // Pointy fox ears
      triangle(cx - r * 0.54F, y - r * 0.16F, cx - r * 0.44F, y - r * 0.78F, cx - r * 0.08F, y - r * 0.44F, fur, 2.0F);
      triangle(cx - r * 0.48F, y - r * 0.56F, cx - r * 0.44F, y - r * 0.78F, cx - r * 0.30F, y - r * 0.62F, tip, 1.0F);
      triangle(cx + r * 0.54F, y - r * 0.16F, cx + r * 0.44F, y - r * 0.78F, cx + r * 0.08F, y - r * 0.44F, fur, 2.0F);
      triangle(cx + r * 0.48F, y - r * 0.56F, cx + r * 0.44F, y - r * 0.78F, cx + r * 0.30F, y - r * 0.62F, tip, 1.0F);

      // Head & fluffy cheeks
      ellipse(cx, y + r * 0.08F, r * 0.62F, r * 0.52F, fur);
      ellipse(cx - r * 0.28F, y + r * 0.28F, r * 0.32F, r * 0.22F, cream);
      ellipse(cx + r * 0.28F, y + r * 0.28F, r * 0.32F, r * 0.22F, cream);
      ellipse(cx, y + r * 0.34F, r * 0.24F, r * 0.19F, cream);

      draw_eye(cx - r * 0.22F, y + r * 0.02F, r * 0.090F);
      draw_eye(cx + r * 0.22F, y + r * 0.02F, r * 0.090F);
      ellipse(cx - r * 0.36F, y + r * 0.18F, r * 0.09F, r * 0.055F, cheek);
      ellipse(cx + r * 0.36F, y + r * 0.18F, r * 0.09F, r * 0.055F, cheek);
      triangle(cx - r * 0.09F, y + r * 0.22F, cx + r * 0.09F, y + r * 0.22F, cx, y + r * 0.32F, tip, 1.2F);
      break;
    }

    case CompanionId::moss: {
      // Calm sage-green forest owl
      const Color feather = hex(0x477654);
      const Color disc = hex(0xE1ECE0);
      const Color gold = hex(0xE4B24B);

      // Little ear tufts
      triangle(cx - r * 0.46F, y - r * 0.34F, cx - r * 0.56F, y - r * 0.70F, cx - r * 0.18F, y - r * 0.50F, feather, 1.5F);
      triangle(cx + r * 0.46F, y - r * 0.34F, cx + r * 0.56F, y - r * 0.70F, cx + r * 0.18F, y - r * 0.50F, feather, 1.5F);

      // Plump body/head
      ellipse(cx, y + r * 0.06F, r * 0.62F, r * 0.60F, feather);
      ellipse(cx, y + r * 0.34F, r * 0.40F, r * 0.26F, disc.with_alpha(0.75F));

      // Twin facial discs (cute owl glasses feel)
      circle(cx - r * 0.23F, y - r * 0.02F, r * 0.24F, disc, hex(0x365B40), std::max(1.2F, r * 0.04F));
      circle(cx + r * 0.23F, y - r * 0.02F, r * 0.24F, disc, hex(0x365B40), std::max(1.2F, r * 0.04F));
      draw_eye(cx - r * 0.23F, y - r * 0.02F, r * 0.105F);
      draw_eye(cx + r * 0.23F, y - r * 0.02F, r * 0.105F);

      // Golden diamond beak
      triangle(cx - r * 0.10F, y + r * 0.11F, cx + r * 0.10F, y + r * 0.11F, cx, y + r * 0.26F, gold, 1.0F);
      break;
    }

    case CompanionId::fern: {
      // Sleek slate-blue cat
      const Color coat = hex(0x4F738B);
      const Color inner = hex(0xDFC7B8);
      const Color snout = hex(0xEEF3F5);

      triangle(cx - r * 0.52F, y - r * 0.18F, cx - r * 0.46F, y - r * 0.72F, cx - r * 0.10F, y - r * 0.42F, coat, 2.0F);
      triangle(cx - r * 0.42F, y - r * 0.30F, cx - r * 0.40F, y - r * 0.58F, cx - r * 0.20F, y - r * 0.40F, inner, 1.0F);
      triangle(cx + r * 0.52F, y - r * 0.18F, cx + r * 0.46F, y - r * 0.72F, cx + r * 0.10F, y - r * 0.42F, coat, 2.0F);
      triangle(cx + r * 0.42F, y - r * 0.30F, cx + r * 0.40F, y - r * 0.58F, cx + r * 0.20F, y - r * 0.40F, inner, 1.0F);

      ellipse(cx, y + r * 0.08F, r * 0.60F, r * 0.54F, coat);
      ellipse(cx, y + r * 0.24F, r * 0.32F, r * 0.22F, snout);
      draw_eye(cx - r * 0.22F, y - r * 0.02F, r * 0.095F);
      draw_eye(cx + r * 0.22F, y - r * 0.02F, r * 0.095F);
      triangle(cx - r * 0.07F, y + r * 0.14F, cx + r * 0.07F, y + r * 0.14F, cx, y + r * 0.22F, hex(0xD58A7E), 1.0F);

      // Delicate whiskers
      const float ws = std::max(1.1F, r * 0.038F);
      line(cx - r * 0.36F, y + r * 0.18F, cx - r * 0.66F, y + r * 0.14F, ws, white.with_alpha(0.85F));
      line(cx - r * 0.36F, y + r * 0.26F, cx - r * 0.64F, y + r * 0.30F, ws, white.with_alpha(0.85F));
      line(cx + r * 0.36F, y + r * 0.18F, cx + r * 0.66F, y + r * 0.14F, ws, white.with_alpha(0.85F));
      line(cx + r * 0.36F, y + r * 0.26F, cx + r * 0.64F, y + r * 0.30F, ws, white.with_alpha(0.85F));
      break;
    }

    case CompanionId::pebble: {
      // Sturdy warm chestnut hedgehog guardian
      const Color quills = hex(0x7B5332);
      const Color face = hex(0xF5E5CE);
      const Color ear = hex(0xA4754B);

      // Cute quill crown behind head
      for (int i = 0; i < 7; ++i) {
        const float a = kPi * (0.95F + static_cast<float>(i) * 0.183F);
        const float bx = cx + std::cos(a) * r * 0.42F;
        const float by = y + std::sin(a) * r * 0.38F;
        const float tx = cx + std::cos(a) * r * 0.78F;
        const float ty = y + std::sin(a) * r * 0.74F;
        const float px = -std::sin(a) * r * 0.16F;
        const float py = std::cos(a) * r * 0.16F;
        triangle(bx + px, by + py, bx - px, by - py, tx, ty, quills, 1.4F);
      }

      circle(cx - r * 0.44F, y - r * 0.16F, r * 0.15F, ear);
      circle(cx + r * 0.44F, y - r * 0.16F, r * 0.15F, ear);
      ellipse(cx, y + r * 0.12F, r * 0.56F, r * 0.48F, face);

      draw_eye(cx - r * 0.20F, y + r * 0.04F, r * 0.090F);
      draw_eye(cx + r * 0.20F, y + r * 0.04F, r * 0.090F);
      ellipse(cx - r * 0.34F, y + r * 0.20F, r * 0.10F, r * 0.06F, cheek);
      ellipse(cx + r * 0.34F, y + r * 0.20F, r * 0.10F, r * 0.06F, cheek);
      circle(cx, y + r * 0.20F, r * 0.075F, hex(0x4A3120));
      arc(cx, y + r * 0.26F, r * 0.13F, kPi * 0.20F, kPi * 0.60F, std::max(1.4F, r * 0.05F), ink);
      break;
    }

    case CompanionId::pip: {
      // Cheerful golden-amber songbird
      const Color body = hex(0xDDA03A);
      const Color belly = hex(0xFBF0D9);
      const Color wing = hex(0xC28224);
      const Color beak = hex(0xDF6D2D);
      const float crest_wag = animate ? std::sin(phase * 4.5F) * (r * 0.05F) : 0.0F;

      // Top feather crest
      ellipse(cx - r * 0.08F + crest_wag, y - r * 0.64F, r * 0.10F, r * 0.18F, wing);
      ellipse(cx + r * 0.08F + crest_wag, y - r * 0.68F, r * 0.09F, r * 0.16F, body);

      // Plump bird body + cream belly + wing
      ellipse(cx, y + r * 0.06F, r * 0.58F, r * 0.56F, body);
      ellipse(cx, y + r * 0.26F, r * 0.40F, r * 0.32F, belly);
      ellipse(cx - r * 0.48F, y + r * 0.14F, r * 0.16F, r * 0.26F, wing);
      ellipse(cx + r * 0.48F, y + r * 0.14F, r * 0.16F, r * 0.26F, wing);

      draw_eye(cx - r * 0.20F, y - r * 0.06F, r * 0.090F);
      draw_eye(cx + r * 0.20F, y - r * 0.06F, r * 0.090F);
      ellipse(cx - r * 0.32F, y + r * 0.08F, r * 0.09F, r * 0.055F, cheek);
      ellipse(cx + r * 0.32F, y + r * 0.08F, r * 0.09F, r * 0.055F, cheek);
      triangle(cx - r * 0.11F, y + r * 0.04F, cx + r * 0.11F, y + r * 0.04F, cx, y + r * 0.20F, beak, 1.0F);
      break;
    }
  }
}

}  // namespace orangutan::vkui
