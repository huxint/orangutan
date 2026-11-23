#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "agent_bridge.hpp"
#include "font_atlas.hpp"
#include "primitives.hpp"
#include "theme.hpp"

namespace orangutan::vkui {

enum class NavView : std::uint8_t {
  chat = 0,
  companions,
  memory,
  reminders,
  settings,
};

enum class InputField : std::uint8_t {
  none = 0,
  chat_composer,
  memory_title,
  memory_body,
  reminder_title,
};

struct UiConfig {
  bool dark_mode{false};
  bool animate_companions{true};
  NavView initial_view{NavView::chat};
  std::string gpu_name{"Vulkan"};
};

class DesktopApp {
public:
  DesktopApp(AgentBridge& bridge, UiConfig cfg = {});

  void set_view(NavView v) noexcept {
    view_ = v;
    scroll_y_ = 0.0F;
  }
  void set_dark_mode(bool dark) noexcept {
    dark_mode_ = dark;
  }
  void set_gpu_name(std::string name) {
    gpu_name_ = std::move(name);
  }
  [[nodiscard]] const Theme& theme() const noexcept {
    return dark_mode_ ? dark_ : light_;
  }

  // Input events (logical points, origin top-left)
  void on_mouse_move(float x, float y);
  void on_mouse_down(float x, float y);
  void on_mouse_up(float x, float y);
  void on_mouse_wheel(float dy);
  void on_text_input(std::string_view utf8);
  void on_text_editing(std::string_view composition);
  void on_backspace();
  void on_enter(bool shift_held);
  void on_paste(std::string_view utf8);

  void tick(float dt_sec);
  void render(DrawList& dl, FontAtlas& atlas, float logical_w, float logical_h, float dpr);

private:
  struct HitAction {
    Rect rect;
    std::function<void()> on_click;
  };

  [[nodiscard]] std::string* active_input_buffer();
  void register_hit(Rect r, std::function<void()> fn);
  void trigger_pet_bounce(CompanionId id);

  void render_sidebar(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, float h);
  void render_chat_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area);
  void render_companions_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area);
  void render_memory_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area);
  void render_reminders_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area);
  void render_settings_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area);

  void draw_button(DrawList& dl, FontAtlas& atlas, Rect r, std::string_view label, bool primary,
                   std::function<void()> on_click, IconKind* icon = nullptr);
  void draw_text_input(DrawList& dl, FontAtlas& atlas, Rect r, InputField field, const std::string& text,
                       std::string_view placeholder, float font_size = 14.0F);

  AgentBridge& bridge_;
  Theme light_{light_theme()};
  Theme dark_{dark_theme()};
  bool dark_mode_{false};
  bool animate_companions_{true};
  std::string gpu_name_{"Vulkan"};

  NavView view_{NavView::chat};
  InputField focused_{InputField::chat_composer};
  std::string composer_text_;
  std::string ime_composition_;
  std::string mem_title_input_;
  std::string mem_body_input_;
  std::string rem_title_input_;
  int rem_delay_index_{0};  // 0: +30m, 1: +2h, 2: tomorrow

  float mouse_x_{-1.0F};
  float mouse_y_{-1.0F};
  bool mouse_down_{false};
  float scroll_y_{0.0F};
  float max_scroll_y_{0.0F};
  float time_sec_{0.0F};
  float pet_bounces_[6]{0, 0, 0, 0, 0, 0};
  std::string pet_toast_;
  float pet_toast_timer_{0.0F};

  std::vector<HitAction> hits_;
};

}  // namespace orangutan::vkui
