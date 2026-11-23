#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "theme.hpp"

namespace orangutan::vkui {

struct Rect {
  float x{0.0F};
  float y{0.0F};
  float w{0.0F};
  float h{0.0F};

  [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
    return px >= x && py >= y && px <= x + w && py <= y + h;
  }

  [[nodiscard]] constexpr Rect inset(float dx, float dy) const noexcept {
    return Rect{x + dx, y + dy, w - dx * 2.0F, h - dy * 2.0F};
  }
};

// Matches std430 UiInstance in shaders/ui.vert & ui.frag (112 bytes).
struct alignas(16) UiInstance {
  float rect[4]{0.0F, 0.0F, 0.0F, 0.0F};
  float clip[4]{0.0F, 0.0F, 16384.0F, 16384.0F};
  float radii[4]{0.0F, 0.0F, 0.0F, 0.0F};
  float color[4]{0.0F, 0.0F, 0.0F, 0.0F};
  float color2[4]{0.0F, 0.0F, 0.0F, 0.0F};
  float uv[4]{0.0F, 0.0F, 0.0F, 0.0F};
  float params[4]{0.0F, 0.0F, 0.0F, 1.0F};
};

static_assert(sizeof(UiInstance) == 112);

enum class IconKind : std::uint8_t {
  chat,
  memory,
  bell,
  sparkles,
  settings,
  plus,
  send,
  stop,
  sun,
  moon,
  check,
  trash,
  shield,
  split_tasks,
  folder,
};

struct CompanionProfile {
  CompanionId id;
  std::string_view key;
  std::string_view name;
  std::string_view nickname;
  std::string_view role_title;
  std::string_view task_focus;
  std::string_view greeting;
  Color accent;
  Color soft;
};

[[nodiscard]] const CompanionProfile& companion_profile(CompanionId id) noexcept;

class DrawList {
public:
  void clear(float viewport_w, float viewport_h, float dpr = 1.0F) noexcept;

  void push_clip(Rect r);
  void pop_clip();

  void rect(Rect r, float radius, Color fill, Color border = Color{0, 0, 0, 0}, float border_w = 0.0F);
  void rect_radii(Rect r, float tl, float tr, float br, float bl, Color fill, Color border = Color{0, 0, 0, 0},
                  float border_w = 0.0F);
  void rect_gradient(Rect r, float radius, Color top, Color bottom);
  void shadow(Rect r, float radius, float blur, float offset_y, Color color);
  void circle(float cx, float cy, float radius, Color fill, Color border = Color{0, 0, 0, 0}, float border_w = 0.0F);
  void ellipse(float cx, float cy, float rx, float ry, Color fill, Color border = Color{0, 0, 0, 0},
               float border_w = 0.0F);
  void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Color fill, float round_r = 0.0F);
  void line(float x0, float y0, float x1, float y1, float thickness, Color color);
  void arc(float cx, float cy, float radius, float start_rad, float sweep_rad, float thickness, Color color);
  void glyph(Rect r, float u0, float v0, float u1, float v1, Color color, float gamma = 0.88F);

  void icon(IconKind kind, float cx, float cy, float size, Color color);
  void companion(CompanionId id, float cx, float cy, float radius, CompanionMood mood, float time_sec,
                 bool animate = true, float pet_bounce = 0.0F);

  [[nodiscard]] const std::vector<UiInstance>& instances() const noexcept {
    return instances_;
  }
  [[nodiscard]] float dpr() const noexcept {
    return dpr_;
  }

private:
  [[nodiscard]] Rect current_clip_px() const noexcept;
  void emit(UiInstance inst);

  float dpr_{1.0F};
  Rect root_clip_{0.0F, 0.0F, 16384.0F, 16384.0F};
  std::vector<Rect> clip_stack_;
  std::vector<UiInstance> instances_;
};

}  // namespace orangutan::vkui
