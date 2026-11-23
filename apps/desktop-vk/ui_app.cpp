#include "ui_app.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <utility>

namespace orangutan::vkui {
namespace {

constexpr float kSidebarW = 252.0F;

void pop_utf8_char(std::string& s) {
  if (s.empty()) {
    return;
  }
  while (!s.empty()) {
    const auto b = static_cast<std::uint8_t>(s.back());
    s.pop_back();
    if ((b & 0xC0U) != 0x80U) {
      break;
    }
  }
}

[[nodiscard]] std::int64_t now_ms() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string format_due_label(std::int64_t due_ms) {
  const std::time_t t = static_cast<std::time_t>(due_ms / 1000);
  std::tm tm_val{};
  localtime_r(&t, &tm_val);
  return std::format("{:02d}/{:02d} {:02d}:{:02d}", tm_val.tm_mon + 1, tm_val.tm_mday, tm_val.tm_hour, tm_val.tm_min);
}

[[nodiscard]] std::string_view subtask_status_zh(std::string_view s) noexcept {
  if (s == "done") {
    return "已完成";
  }
  if (s == "waiting") {
    return "待确认";
  }
  return "推进中";
}

}  // namespace

DesktopApp::DesktopApp(AgentBridge& bridge, UiConfig cfg)
    : bridge_{bridge},
      dark_mode_{cfg.dark_mode},
      animate_companions_{cfg.animate_companions},
      gpu_name_{std::move(cfg.gpu_name)},
      view_{cfg.initial_view} {}

std::string* DesktopApp::active_input_buffer() {
  switch (focused_) {
    case InputField::chat_composer:
      return &composer_text_;
    case InputField::memory_title:
      return &mem_title_input_;
    case InputField::memory_body:
      return &mem_body_input_;
    case InputField::reminder_title:
      return &rem_title_input_;
    case InputField::none:
      return nullptr;
  }
  return nullptr;
}

void DesktopApp::register_hit(Rect r, std::function<void()> fn) {
  hits_.push_back(HitAction{r, std::move(fn)});
}

void DesktopApp::trigger_pet_bounce(CompanionId id) {
  const auto idx = static_cast<std::size_t>(id);
  if (idx < 6U) {
    pet_bounces_[idx] = 1.0F;
    const auto& p = companion_profile(id);
    pet_toast_ = std::format("{}：{}", p.nickname, p.greeting);
    pet_toast_timer_ = 4.5F;
  }
}

void DesktopApp::on_mouse_move(float x, float y) {
  mouse_x_ = x;
  mouse_y_ = y;
}

void DesktopApp::on_mouse_down(float x, float y) {
  mouse_x_ = x;
  mouse_y_ = y;
  mouse_down_ = true;
}

void DesktopApp::on_mouse_up(float x, float y) {
  mouse_x_ = x;
  mouse_y_ = y;
  if (!mouse_down_) {
    return;
  }
  mouse_down_ = false;
  // Walk top-most hits first.
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
    if (it->rect.contains(x, y)) {
      if (it->on_click) {
        it->on_click();
      }
      return;
    }
  }
}

void DesktopApp::on_mouse_wheel(float dy) {
  scroll_y_ = std::clamp(scroll_y_ - dy * 42.0F, 0.0F, max_scroll_y_);
}

void DesktopApp::on_text_input(std::string_view utf8) {
  ime_composition_.clear();
  if (auto* buf = active_input_buffer()) {
    buf->append(utf8);
  }
}

void DesktopApp::on_text_editing(std::string_view composition) {
  ime_composition_ = std::string{composition};
}

void DesktopApp::on_backspace() {
  if (!ime_composition_.empty()) {
    return;
  }
  if (auto* buf = active_input_buffer()) {
    pop_utf8_char(*buf);
  }
}

void DesktopApp::on_paste(std::string_view utf8) {
  if (auto* buf = active_input_buffer()) {
    buf->append(utf8);
  }
}

void DesktopApp::on_enter(bool shift_held) {
  if (focused_ == InputField::chat_composer) {
    if (shift_held) {
      composer_text_.push_back('\n');
      return;
    }
    if (!composer_text_.empty()) {
      auto text = std::move(composer_text_);
      composer_text_.clear();
      bridge_.send_message(std::move(text));
    }
  } else if (focused_ == InputField::memory_title) {
    focused_ = InputField::memory_body;
  } else if (focused_ == InputField::memory_body) {
    if (!mem_title_input_.empty() && !mem_body_input_.empty()) {
      bridge_.add_memory_note(mem_title_input_, mem_body_input_, "personal");
      mem_title_input_.clear();
      mem_body_input_.clear();
    }
  } else if (focused_ == InputField::reminder_title) {
    if (!rem_title_input_.empty()) {
      constexpr std::array<std::int64_t, 3> kOffsetsMs{30LL * 60 * 1000, 120LL * 60 * 1000, 24LL * 3600 * 1000};
      bridge_.add_reminder(rem_title_input_, now_ms() + kOffsetsMs[static_cast<std::size_t>(rem_delay_index_)]);
      rem_title_input_.clear();
    }
  }
}

void DesktopApp::tick(float dt_sec) {
  time_sec_ += dt_sec;
  for (float& b : pet_bounces_) {
    if (b > 0.0F) {
      b = std::max(0.0F, b - dt_sec * 2.1F);
    }
  }
  if (pet_toast_timer_ > 0.0F) {
    pet_toast_timer_ = std::max(0.0F, pet_toast_timer_ - dt_sec);
    if (pet_toast_timer_ <= 0.0F) {
      pet_toast_.clear();
    }
  }
}

void DesktopApp::draw_button(DrawList& dl, FontAtlas& atlas, Rect r, std::string_view label, bool primary,
                             std::function<void()> on_click, IconKind* icon) {
  const auto& th = theme();
  const bool hov = r.contains(mouse_x_, mouse_y_);
  const Color bg = primary ? (hov ? th.accent_hover : th.accent) : (hov ? th.hover : th.raised);
  const Color border = primary ? Color{0, 0, 0, 0} : (hov ? th.line_strong : th.line);
  const Color fg = primary ? th.on_accent : th.text;

  if (primary) {
    dl.shadow(r, 10.0F, 8.0F, 2.0F, th.accent.with_alpha(dark_mode_ ? 0.25F : 0.20F));
  }
  dl.rect(r, 10.0F, bg, border, primary ? 0.0F : 1.0F);

  const float font_pt = 13.5F;
  const float tw = atlas.measure_line(label, FontRole::ui_bold, font_pt);
  const float icon_space = icon != nullptr ? 20.0F : 0.0F;
  const float total_w = tw + icon_space;
  float x = r.x + (r.w - total_w) * 0.5F;
  const float cy = r.y + r.h * 0.5F;

  if (icon != nullptr) {
    dl.icon(*icon, x + 6.0F, cy, 13.5F, fg);
    x += 20.0F;
  }
  atlas.draw_line(dl, x, cy - font_pt * 0.68F, label, FontRole::ui_bold, font_pt, fg);
  register_hit(r, std::move(on_click));
}

void DesktopApp::draw_text_input(DrawList& dl, FontAtlas& atlas, Rect r, InputField field, const std::string& text,
                                 std::string_view placeholder, float font_size) {
  const auto& th = theme();
  const bool active = focused_ == field;
  const bool hov = r.contains(mouse_x_, mouse_y_);
  const Color border = active ? th.accent_line : (hov ? th.line_strong : th.line);

  if (active) {
    dl.rect(r.inset(-2.0F, -2.0F), 12.0F, th.accent.with_alpha(0.11F));
  }
  dl.rect(r, 10.0F, th.raised, border, active ? 1.5F : 1.0F);

  const std::string display = active ? (text + ime_composition_) : text;
  const float tx = r.x + 14.0F;
  const float ty = r.y + (r.h - font_size * 1.35F) * 0.5F;

  dl.push_clip(r.inset(10.0F, 4.0F));
  if (display.empty()) {
    atlas.draw_line(dl, tx, ty, placeholder, FontRole::ui, font_size, th.muted);
  } else {
    const float w = atlas.draw_line(dl, tx, ty, display, FontRole::ui, font_size, th.text);
    if (active && std::fmod(time_sec_, 1.0F) < 0.55F) {
      dl.rect(Rect{tx + w + 1.5F, ty + 2.0F, 1.8F, font_size * 1.15F}, 1.0F, th.accent);
    }
  }
  dl.pop_clip();

  register_hit(r, [this, field]() { focused_ = field; });
}

void DesktopApp::render(DrawList& dl, FontAtlas& atlas, float logical_w, float logical_h, float dpr) {
  hits_.clear();
  dl.clear(logical_w, logical_h, dpr);

  const auto snap = bridge_.snapshot();
  render_sidebar(dl, atlas, snap, logical_h);

  const Rect main_area{kSidebarW, 0.0F, std::max(logical_w - kSidebarW, 320.0F), logical_h};
  switch (view_) {
    case NavView::chat:
      render_chat_view(dl, atlas, snap, main_area);
      break;
    case NavView::companions:
      render_companions_view(dl, atlas, snap, main_area);
      break;
    case NavView::memory:
      render_memory_view(dl, atlas, snap, main_area);
      break;
    case NavView::reminders:
      render_reminders_view(dl, atlas, snap, main_area);
      break;
    case NavView::settings:
      render_settings_view(dl, atlas, snap, main_area);
      break;
  }

  // Gentle floating companion toast if user petted/greeted a companion
  if (!pet_toast_.empty() && pet_toast_timer_ > 0.0F) {
    const auto& th = theme();
    const float tw = std::min(atlas.measure_line(pet_toast_, FontRole::ui, 13.0F) + 32.0F, main_area.w - 48.0F);
    const Rect toast{main_area.x + (main_area.w - tw) * 0.5F, 18.0F, tw, 36.0F};
    dl.shadow(toast, 18.0F, 14.0F, 4.0F, th.shadow);
    dl.rect(toast, 18.0F, th.raised, th.accent_line, 1.0F);
    atlas.draw_line(dl, toast.x + 16.0F, toast.y + 9.0F, pet_toast_, FontRole::ui, 13.0F, th.text, tw - 32.0F, true);
  }
}

void DesktopApp::render_sidebar(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, float h) {
  const auto& th = theme();
  dl.rect(Rect{0.0F, 0.0F, kSidebarW, h}, 0.0F, th.sidebar);
  dl.rect(Rect{kSidebarW - 1.0F, 0.0F, 1.0F, h}, 0.0F, th.line);

  // Top brand header
  const float lead_bounce = pet_bounces_[static_cast<std::size_t>(snap.lead_companion)];
  dl.companion(snap.lead_companion, 34.0F, 34.0F, 17.0F, snap.active_mood, time_sec_, animate_companions_, lead_bounce);
  register_hit(Rect{14.0F, 14.0F, 40.0F, 40.0F}, [this, id = snap.lead_companion]() { trigger_pet_bounce(id); });

  atlas.draw_line(dl, 60.0F, 19.0F, "Orangutan", FontRole::display, 18.5F, th.text);
  const std::string_view badge = snap.demo_mode ? "本地引擎 · Demo" : "本地引擎 · Live";
  atlas.draw_line(dl, 60.0F, 41.0F, badge, FontRole::ui, 11.0F, th.muted);

  // "+ 新对话" primary action
  IconKind plus_icon = IconKind::plus;
  draw_button(
      dl, atlas, Rect{16.0F, 68.0F, kSidebarW - 32.0F, 38.0F}, "新建对话", true,
      [this]() {
        bridge_.new_conversation();
        view_ = NavView::chat;
        focused_ = InputField::chat_composer;
        scroll_y_ = 0.0F;
      },
      &plus_icon);

  // Primary navigation items
  struct NavDef {
    NavView id;
    IconKind icon;
    std::string_view label;
    std::size_t count;
  };
  const std::array<NavDef, 5> navs{{
      {NavView::chat, IconKind::chat, "对话协作", 0},
      {NavView::companions, IconKind::sparkles, "伙伴小队", 6},
      {NavView::memory, IconKind::memory, "长期记忆", snap.notes.size()},
      {NavView::reminders, IconKind::bell, "轻量提醒", snap.reminders.size()},
      {NavView::settings, IconKind::settings, "偏好设置", 0},
  }};

  float y = 120.0F;
  for (const auto& item : navs) {
    const Rect row{12.0F, y, kSidebarW - 24.0F, 36.0F};
    const bool selected = view_ == item.id;
    const bool hov = row.contains(mouse_x_, mouse_y_);
    if (selected) {
      dl.rect(row, 9.0F, th.selected);
    } else if (hov) {
      dl.rect(row, 9.0F, th.sidebar_hover);
    }
    const Color fg = selected ? th.selected_text : th.subtext;
    dl.icon(item.icon, row.x + 19.0F, row.y + row.h * 0.5F, 14.5F, selected ? th.accent : th.muted);
    atlas.draw_line(dl, row.x + 36.0F, row.y + 9.0F, item.label, selected ? FontRole::ui_bold : FontRole::ui, 13.5F, fg);
    if (item.count > 0) {
      const auto badge_str = std::to_string(item.count);
      const float bw = std::max(22.0F, atlas.measure_line(badge_str, FontRole::ui_bold, 11.0F) + 12.0F);
      const Rect pill{row.x + row.w - bw - 8.0F, row.y + 8.0F, bw, 20.0F};
      dl.rect(pill, 10.0F, selected ? th.accent.with_alpha(0.16F) : th.panel);
      const float tw = atlas.measure_line(badge_str, FontRole::ui_bold, 11.0F);
      atlas.draw_line(dl, pill.x + (pill.w - tw) * 0.5F, pill.y + 3.5F, badge_str, FontRole::ui_bold, 11.0F,
                      selected ? th.selected_text : th.muted);
    }
    register_hit(row, [this, target = item.id]() {
      view_ = target;
      scroll_y_ = 0.0F;
      if (target == NavView::chat) {
        focused_ = InputField::chat_composer;
      }
    });
    y += 40.0F;
  }

  // Recent conversations list
  y += 10.0F;
  dl.rect(Rect{16.0F, y, kSidebarW - 32.0F, 1.0F}, 0.0F, th.line_soft);
  y += 14.0F;
  atlas.draw_line(dl, 20.0F, y, "最近对话", FontRole::ui_bold, 11.5F, th.muted);
  y += 24.0F;

  const float bottom_card_h = 90.0F;
  const float conv_bottom = h - bottom_card_h - 16.0F;
  dl.push_clip(Rect{8.0F, y, kSidebarW - 16.0F, std::max(0.0F, conv_bottom - y)});
  for (const auto& conv : snap.conversations) {
    if (y + 32.0F > conv_bottom) {
      break;
    }
    const Rect r{12.0F, y, kSidebarW - 24.0F, 32.0F};
    const bool active = view_ == NavView::chat && conv.id == snap.active_conversation_id;
    const bool hov = r.contains(mouse_x_, mouse_y_);
    if (active) {
      dl.rect(r, 8.0F, th.raised, th.line_soft, 1.0F);
    } else if (hov) {
      dl.rect(r, 8.0F, th.sidebar_hover);
    }
    atlas.draw_line(dl, r.x + 12.0F, r.y + 7.5F, conv.title, active ? FontRole::ui_bold : FontRole::ui, 12.5F,
                    active ? th.text : th.subtext, r.w - 22.0F, true);
    register_hit(r, [this, cid = conv.id]() {
      bridge_.select_conversation(cid);
      view_ = NavView::chat;
      scroll_y_ = 0.0F;
    });
    y += 36.0F;
  }
  dl.pop_clip();

  // Bottom Active Companion Card + Light/Dark Theme Switch
  const Rect card{12.0F, h - bottom_card_h - 12.0F, kSidebarW - 24.0F, bottom_card_h};
  dl.shadow(card, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.6F));
  dl.rect(card, 14.0F, th.surface, th.line, 1.0F);

  const auto& lead_prof = companion_profile(snap.lead_companion);
  dl.companion(snap.lead_companion, card.x + 32.0F, card.y + 36.0F, 20.0F, snap.active_mood, time_sec_,
               animate_companions_, lead_bounce);
  register_hit(Rect{card.x + 8.0F, card.y + 10.0F, 48.0F, 52.0F},
               [this, id = snap.lead_companion]() { trigger_pet_bounce(id); });

  const auto title = std::format("{} · {}", lead_prof.name, lead_prof.nickname);
  atlas.draw_line(dl, card.x + 60.0F, card.y + 16.0F, title, FontRole::ui_bold, 13.0F, th.text, card.w - 102.0F, true);
  atlas.draw_line(dl, card.x + 60.0F, card.y + 36.0F, lead_prof.role_title, FontRole::ui, 11.5F, lead_prof.accent,
                  card.w - 102.0F, true);
  atlas.draw_line(dl, card.x + 14.0F, card.y + 64.0F, snap.status_line, FontRole::ui, 11.5F, th.muted, card.w - 28.0F,
                  true);

  // Light / Dark theme toggle icon button
  const Rect theme_btn{card.x + card.w - 36.0F, card.y + 14.0F, 26.0F, 26.0F};
  const bool th_hov = theme_btn.contains(mouse_x_, mouse_y_);
  dl.rect(theme_btn, 8.0F, th_hov ? th.hover : th.panel);
  dl.icon(dark_mode_ ? IconKind::sun : IconKind::moon, theme_btn.x + 13.0F, theme_btn.y + 13.0F, 13.0F, th.subtext);
  register_hit(theme_btn, [this]() { dark_mode_ = !dark_mode_; });
}

void DesktopApp::render_chat_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area) {
  const auto& th = theme();
  const float col_w = std::min(780.0F, area.w - 64.0F);
  const float col_x = area.x + (area.w - col_w) * 0.5F;

  // Subtle top bar
  const float top_h = 54.0F;
  dl.rect(Rect{area.x, area.y, area.w, top_h}, 0.0F, th.bg);
  dl.rect(Rect{area.x, area.y + top_h - 1.0F, area.w, 1.0F}, 0.0F, th.line_soft);

  std::string conv_title = "对话协作";
  for (const auto& c : snap.conversations) {
    if (c.id == snap.active_conversation_id) {
      conv_title = c.title;
      break;
    }
  }
  atlas.draw_line(dl, col_x, area.y + 16.0F, conv_title, FontRole::ui_bold, 15.0F, th.text, col_w - 190.0F, true);

  // Model + Vulkan pill on top right
  const std::string pill_text = snap.demo_mode ? "Vulkan · 本地 Demo" : snap.model_name;
  const float pw = atlas.measure_line(pill_text, FontRole::ui, 11.5F) + 22.0F;
  const Rect model_pill{col_x + col_w - pw, area.y + 14.0F, pw, 26.0F};
  dl.rect(model_pill, 13.0F, th.surface, th.line, 1.0F);
  dl.circle(model_pill.x + 10.0F, model_pill.y + 13.0F, 3.5F, snap.busy ? th.accent : th.green);
  atlas.draw_line(dl, model_pill.x + 18.0F, model_pill.y + 5.5F, pill_text, FontRole::ui, 11.5F, th.subtext);

  // Composer dock geometry at bottom
  const float composer_h = 64.0F;
  const float footer_h = composer_h + 44.0F;
  const Rect scroll_area{area.x, area.y + top_h, area.w, std::max(120.0F, area.h - top_h - footer_h)};

  dl.push_clip(scroll_area);

  if (snap.messages.empty()) {
    max_scroll_y_ = 0.0F;
    scroll_y_ = 0.0F;
    const auto& lead = companion_profile(snap.lead_companion);
    const float bounce = pet_bounces_[static_cast<std::size_t>(snap.lead_companion)];
    const float hero_top = scroll_area.y + std::max(28.0F, (scroll_area.h - 400.0F) * 0.42F);
    const float center_x = col_x + col_w * 0.5F;

    // Large welcoming animated photorealistic companion portrait
    const Rect hero_hit{center_x - 58.0F, hero_top - 4.0F, 116.0F, 116.0F};
    const float hero_hov = hero_hit.contains(mouse_x_, mouse_y_) ? 1.0F : 0.0F;
    dl.companion(snap.lead_companion, center_x, hero_top + 52.0F, 52.0F, snap.active_mood, time_sec_,
                 animate_companions_, bounce, hero_hov);
    register_hit(hero_hit, [this, id = snap.lead_companion]() { trigger_pet_bounce(id); });

    const std::string greeting = std::format("你好，我是 {}（{}）", lead.name, lead.nickname);
    const float gw = atlas.measure_line(greeting, FontRole::display, 30.0F);
    atlas.draw_line(dl, center_x - gw * 0.5F, hero_top + 116.0F, greeting, FontRole::display, 30.0F, th.text);

    const std::string_view sub = "你的本地 Personal Agent —— 陪你自然对话，把复杂目标拆给伙伴小队并行推进";
    const float sw = atlas.measure_line(sub, FontRole::ui, 14.0F);
    atlas.draw_line(dl, center_x - sw * 0.5F, hero_top + 160.0F, sub, FontRole::ui, 14.0F, th.subtext);

    // 2x2 Quick-start task cards
    struct Starter {
      CompanionId who;
      std::string_view title;
      std::string_view desc;
      std::string_view prompt;
    };
    const std::array<Starter, 4> starters{{
        {CompanionId::oran, "把一个目标拆给小队推进", "调研现状、起草计划并安排跟进提醒",
         "帮我把本周发布准备拆给小队推进：先浏览工作区现状，起草清单，并记下偏好"},
        {CompanionId::scout, "摸清当前工作区结构", "快速浏览目录与关键文件脉络", "列出当前工作区文件"},
        {CompanionId::moss, "记下我的个人偏好", "沉淀到本机 SQLite 跨会话生效", "记住：以后用中文先给我结论，再展开理由"},
        {CompanionId::pip, "安排一个轻量提醒", "写进本地 personal.db 到点轻声提醒",
         "帮我记一个今晚 20:00 复查待办清单的提醒"},
    }};

    const float gap = 14.0F;
    const float card_w = (col_w - gap) * 0.5F;
    const float card_h = 84.0F;
    const float grid_y = hero_top + 196.0F;
    for (std::size_t i = 0; i < starters.size(); ++i) {
      const float cx = col_x + static_cast<float>(i % 2U) * (card_w + gap);
      const float cy = grid_y + static_cast<float>(i / 2U) * (card_h + gap);
      const Rect cr{cx, cy, card_w, card_h};
      const bool hov = cr.contains(mouse_x_, mouse_y_);
      dl.shadow(cr, 14.0F, hov ? 14.0F : 8.0F, hov ? 4.0F : 2.0F, th.shadow);
      dl.rect(cr, 14.0F, th.raised, hov ? th.accent_line : th.line, 1.0F);

      dl.companion(starters[i].who, cr.x + 30.0F, cr.y + 42.0F, 16.0F, CompanionMood::idle, time_sec_,
                   animate_companions_);
      atlas.draw_line(dl, cr.x + 56.0F, cr.y + 19.0F, starters[i].title, FontRole::ui_bold, 13.5F, th.text,
                      cr.w - 72.0F, true);
      atlas.draw_line(dl, cr.x + 56.0F, cr.y + 44.0F, starters[i].desc, FontRole::ui, 12.0F, th.muted, cr.w - 72.0F,
                      true);
      register_hit(cr, [this, p = std::string{starters[i].prompt}]() { bridge_.send_message(p); });
    }
  } else {
    float y = scroll_area.y + 24.0F - scroll_y_;

    for (const auto& msg : snap.messages) {
      if (msg.is_user) {
        const float max_bw = col_w * 0.72F;
        const auto m = atlas.measure_wrapped(msg.text, FontRole::ui, 14.5F, max_bw - 32.0F);
        const float bw = std::clamp(m.width + 32.0F, 64.0F, max_bw);
        const float bh = m.height + 24.0F;
        const Rect bubble{col_x + col_w - bw, y, bw, bh};
        dl.rect(bubble, 16.0F, th.sidebar, th.line_soft, 1.0F);
        atlas.draw_wrapped(dl, bubble.x + 16.0F, bubble.y + 12.0F, msg.text, FontRole::ui, 14.5F, th.text, bw - 32.0F);
        y += bh + 22.0F;
      } else {
        const auto& prof = companion_profile(msg.speaker);
        dl.companion(msg.speaker, col_x + 18.0F, y + 18.0F, 16.0F,
                     msg.streaming ? CompanionMood::working : CompanionMood::idle, time_sec_, animate_companions_);
        const auto speaker_label = std::format("{} · {}", prof.name, prof.nickname);
        atlas.draw_line(dl, col_x + 44.0F, y + 4.0F, speaker_label, FontRole::ui_bold, 13.0F, th.text);
        atlas.draw_line(dl, col_x + 44.0F + atlas.measure_line(speaker_label, FontRole::ui_bold, 13.0F) + 10.0F,
                        y + 5.0F, prof.role_title, FontRole::ui, 11.5F, th.muted);

        const float body_x = col_x + 44.0F;
        const float body_w = col_w - 52.0F;
        const std::string shown = msg.text.empty() && msg.streaming ? "正在思考并拆解任务…" : msg.text;
        const auto bm = atlas.draw_wrapped(dl, body_x, y + 28.0F, shown, FontRole::ui, 14.5F,
                                           msg.text.empty() ? th.muted : th.text, body_w);
        if (msg.streaming && std::fmod(time_sec_, 0.9F) < 0.5F) {
          dl.rect(Rect{body_x, y + 28.0F + bm.height + 4.0F, 8.0F, 15.0F}, 2.0F, th.accent);
        }
        y += 28.0F + bm.height + 24.0F;
      }
    }

    // Team Sub-Task Split Card (splitting tasks across teammates, NOT tools!)
    if (!snap.subtasks.empty()) {
      const std::size_t rows = (snap.subtasks.size() + 1U) / 2U;
      const float card_inner_h = 46.0F + static_cast<float>(rows) * 68.0F + 10.0F;
      const Rect board{col_x + 44.0F, y, col_w - 44.0F, card_inner_h};
      dl.shadow(board, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.65F));
      dl.rect(board, 14.0F, th.surface, th.line, 1.0F);

      dl.icon(IconKind::split_tasks, board.x + 22.0F, board.y + 24.0F, 15.0F, th.accent);
      atlas.draw_line(dl, board.x + 38.0F, board.y + 15.0F, "伙伴小队任务分工", FontRole::ui_bold, 13.0F, th.text);
      atlas.draw_line(dl, board.x + 154.0F, board.y + 16.5F, "按子任务并行协作", FontRole::ui, 11.5F, th.muted);

      const float pad = 14.0F;
      const float gap = 10.0F;
      const float cell_w = (board.w - pad * 2.0F - gap) * 0.5F;
      const float cell_h = 58.0F;
      for (std::size_t i = 0; i < snap.subtasks.size(); ++i) {
        const auto& st = snap.subtasks[i];
        const auto& who = companion_profile(st.assignee);
        const float cx = board.x + pad + static_cast<float>(i % 2U) * (cell_w + gap);
        const float cy = board.y + 44.0F + static_cast<float>(i / 2U) * (cell_h + gap);
        const Rect cell{cx, cy, cell_w, cell_h};
        dl.rect(cell, 11.0F, th.raised, th.line_soft, 1.0F);

        const CompanionMood st_mood =
            st.status == "running" ? CompanionMood::working
                                   : (st.status == "waiting" ? CompanionMood::guarding : CompanionMood::idle);
        dl.companion(st.assignee, cell.x + 24.0F, cell.y + 29.0F, 14.0F, st_mood, time_sec_, animate_companions_);

        // Status pill
        const auto status_label = subtask_status_zh(st.status);
        const Color pill_bg = st.status == "done" ? th.green_soft
                                                  : (st.status == "waiting" ? th.amber_soft : th.accent_soft);
        const Color pill_fg = st.status == "done" ? th.green : (st.status == "waiting" ? th.amber : th.accent);
        const float sw = atlas.measure_line(status_label, FontRole::ui_bold, 10.5F) + 14.0F;
        const Rect sp{cell.x + cell.w - sw - 10.0F, cell.y + 10.0F, sw, 20.0F};
        dl.rect(sp, 10.0F, pill_bg);
        atlas.draw_line(dl, sp.x + 7.0F, sp.y + 3.5F, status_label, FontRole::ui_bold, 10.5F, pill_fg);

        const auto head = std::format("{} · {}", who.nickname, st.title);
        atlas.draw_line(dl, cell.x + 46.0F, cell.y + 11.0F, head, FontRole::ui_bold, 12.5F, th.text,
                        cell.w - sw - 64.0F, true);
        atlas.draw_line(dl, cell.x + 46.0F, cell.y + 33.0F, st.detail, FontRole::ui, 11.5F, th.muted, cell.w - 58.0F,
                        true);
      }
      y += card_inner_h + 18.0F;
    }

    // Interactive Permission Approval Card (Pebble guardian)
    if (snap.pending_approval.has_value()) {
      const auto& ap = *snap.pending_approval;
      const Rect card{col_x + 44.0F, y, col_w - 44.0F, 104.0F};
      dl.shadow(card, 14.0F, 12.0F, 3.0F, th.shadow);
      dl.rect(card, 14.0F, th.amber_soft, th.amber.with_alpha(0.45F), 1.2F);

      dl.companion(CompanionId::pebble, card.x + 34.0F, card.y + 44.0F, 20.0F, CompanionMood::guarding, time_sec_,
                   animate_companions_);
      atlas.draw_line(dl, card.x + 66.0F, card.y + 15.0F,
                      std::format("石墩请求确认：{} ({})", ap.action_name, ap.target), FontRole::ui_bold, 13.5F,
                      th.text, card.w - 84.0F, true);
      atlas.draw_line(dl, card.x + 66.0F, card.y + 38.0F, ap.reason, FontRole::ui, 12.5F, th.subtext, card.w - 84.0F,
                      true);

      draw_button(dl, atlas, Rect{card.x + 66.0F, card.y + 64.0F, 98.0F, 30.0F}, "允许执行", true,
                  [this, id = ap.id]() { bridge_.resolve_approval(id, true); });
      draw_button(dl, atlas, Rect{card.x + 174.0F, card.y + 64.0F, 82.0F, 30.0F}, "拒绝", false,
                  [this, id = ap.id]() { bridge_.resolve_approval(id, false); });
      y += 120.0F;
    }

    const float total_content_h = (y + scroll_y_) - scroll_area.y + 16.0F;
    max_scroll_y_ = std::max(0.0F, total_content_h - scroll_area.h);
    scroll_y_ = std::clamp(scroll_y_, 0.0F, max_scroll_y_);
  }

  dl.pop_clip();

  // Floating elevated Composer
  const Rect comp{col_x, area.y + area.h - footer_h + 10.0F, col_w, composer_h};
  const bool comp_active = focused_ == InputField::chat_composer;
  dl.shadow(comp, 18.0F, 18.0F, 5.0F, th.shadow);
  if (comp_active) {
    dl.rect(comp.inset(-2.0F, -2.0F), 20.0F, th.accent.with_alpha(0.11F));
  }
  dl.rect(comp, 18.0F, th.raised, comp_active ? th.accent_line : th.line, comp_active ? 1.5F : 1.0F);

  // Text inside composer
  const Rect text_rect{comp.x + 18.0F, comp.y + 10.0F, comp.w - 78.0F, comp.h - 20.0F};
  dl.push_clip(text_rect);
  const std::string shown_input = comp_active ? (composer_text_ + ime_composition_) : composer_text_;
  const float ty = comp.y + 20.0F;
  if (shown_input.empty()) {
    atlas.draw_line(dl, text_rect.x, ty, "告诉伙伴小队你想推进什么任务…（回车发送）", FontRole::ui, 14.5F,
                    th.muted);
  } else {
    const float tw = atlas.draw_line(dl, text_rect.x, ty, shown_input, FontRole::ui, 14.5F, th.text);
    if (comp_active && std::fmod(time_sec_, 1.0F) < 0.55F) {
      dl.rect(Rect{text_rect.x + tw + 1.5F, ty + 2.0F, 2.0F, 18.0F}, 1.0F, th.accent);
    }
  }
  dl.pop_clip();
  register_hit(text_rect, [this]() { focused_ = InputField::chat_composer; });

  // Send / Stop circular action button
  const Rect send_btn{comp.x + comp.w - 50.0F, comp.y + 13.0F, 38.0F, 38.0F};
  const bool can_send = !composer_text_.empty();
  const bool send_hov = send_btn.contains(mouse_x_, mouse_y_);
  if (snap.busy) {
    dl.rect(send_btn, 19.0F, send_hov ? th.red : th.text);
    dl.icon(IconKind::stop, send_btn.x + 19.0F, send_btn.y + 19.0F, 14.0F, th.on_accent);
    register_hit(send_btn, [this]() { bridge_.stop_active_turn(); });
  } else {
    const Color bg = can_send ? (send_hov ? th.accent_hover : th.accent) : th.panel;
    const Color fg = can_send ? th.on_accent : th.muted;
    dl.rect(send_btn, 19.0F, bg);
    dl.icon(IconKind::send, send_btn.x + 19.0F, send_btn.y + 19.0F, 15.0F, fg);
    register_hit(send_btn, [this]() {
      if (!composer_text_.empty()) {
        auto text = std::move(composer_text_);
        composer_text_.clear();
        bridge_.send_message(std::move(text));
      }
    });
  }

  // Subtle footer hint
  const std::string_view hint = "Orangutan 纯 Vulkan 原生端 · C++26 本地运行时 · 你的数据仅保留在本机";
  const float hw = atlas.measure_line(hint, FontRole::ui, 11.0F);
  atlas.draw_line(dl, col_x + (col_w - hw) * 0.5F, comp.y + comp.h + 10.0F, hint, FontRole::ui, 11.0F, th.muted);
}

void DesktopApp::render_companions_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area) {
  const auto& th = theme();
  const float col_w = std::min(920.0F, area.w - 64.0F);
  const float col_x = area.x + (area.w - col_w) * 0.5F;
  float y = area.y + 34.0F;

  atlas.draw_line(dl, col_x, y, "伙伴小队", FontRole::display, 28.0F, th.text);
  y += 40.0F;
  atlas.draw_line(dl, col_x, y, "选择你喜欢的伙伴作为主桌宠，点击头像可以互动打招呼 —— 面对复杂目标时，小队会按子任务协作而不是切碎工具。",
                  FontRole::ui, 13.5F, th.subtext, col_w, true);
  y += 38.0F;

  const float gap = 18.0F;
  const float card_w = (col_w - gap * 2.0F) / 3.0F;
  const float card_h = 304.0F;

  for (std::size_t i = 0; i < 6U; ++i) {
    const auto id = static_cast<CompanionId>(i);
    const auto& p = companion_profile(id);
    const bool is_lead = snap.lead_companion == id;
    const float cx = col_x + static_cast<float>(i % 3U) * (card_w + gap);
    const float cy = y + static_cast<float>(i / 3U) * (card_h + gap);
    const Rect card{cx, cy, card_w, card_h};
    const bool hov = card.contains(mouse_x_, mouse_y_);

    dl.shadow(card, 18.0F, hov ? 18.0F : 10.0F, hov ? 5.0F : 2.0F, th.shadow);
    dl.rect(card, 18.0F, th.raised, is_lead ? th.accent : (hov ? th.line_strong : th.line), is_lead ? 1.8F : 1.0F);

    // Tactile studio-macro figurine showcase frame at top of card
    const Rect stage{card.x + 10.0F, card.y + 10.0F, card.w - 20.0F, 134.0F};
    dl.rect(stage, 13.0F, dark_mode_ ? p.accent.with_alpha(0.14F) : p.soft.with_alpha(0.85F));
    const float avatar_cx = stage.x + stage.w * 0.5F;
    const float avatar_cy = stage.y + stage.h * 0.5F;
    dl.companion(id, avatar_cx, avatar_cy, 54.0F, is_lead ? snap.active_mood : CompanionMood::idle, time_sec_,
                 animate_companions_, pet_bounces_[i], hov ? 1.0F : 0.0F);
    register_hit(stage, [this, id]() { trigger_pet_bounce(id); });

    if (is_lead) {
      const Rect tag{card.x + card.w - 86.0F, card.y + 18.0F, 70.0F, 22.0F};
      dl.rect(tag, 11.0F, th.accent);
      atlas.draw_line(dl, tag.x + 10.0F, tag.y + 4.0F, "当前伙伴", FontRole::ui_bold, 10.5F, th.on_accent);
    }

    const auto title = std::format("{} · {}", p.name, p.nickname);
    atlas.draw_line(dl, card.x + 18.0F, card.y + 156.0F, title, FontRole::ui_bold, 15.5F, th.text);

    // Role pill next to title
    const float rw = atlas.measure_line(p.role_title, FontRole::ui_bold, 10.5F) + 16.0F;
    const Rect rpill{card.x + card.w - rw - 16.0F, card.y + 157.0F, rw, 21.0F};
    dl.rect(rpill, 10.5F, dark_mode_ ? p.accent.with_alpha(0.22F) : p.soft);
    atlas.draw_line(dl, rpill.x + 8.0F, rpill.y + 3.5F, p.role_title, FontRole::ui_bold, 10.5F,
                    dark_mode_ ? th.text : p.accent);

    atlas.draw_wrapped(dl, card.x + 18.0F, card.y + 188.0F, p.task_focus, FontRole::ui, 12.4F, th.subtext,
                       card.w - 36.0F);

    // Action footer buttons
    const Rect select_btn{card.x + 16.0F, card.y + card.h - 44.0F, card.w - 96.0F, 30.0F};
    draw_button(dl, atlas, select_btn, is_lead ? "正在陪伴" : "设为伙伴", is_lead, [this, id]() {
      bridge_.set_lead_companion(id);
      trigger_pet_bounce(id);
    });
    const Rect pet_btn{card.x + card.w - 72.0F, card.y + card.h - 44.0F, 56.0F, 30.0F};
    draw_button(dl, atlas, pet_btn, "抚摸", false, [this, id]() { trigger_pet_bounce(id); });
  }
}

void DesktopApp::render_memory_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area) {
  const auto& th = theme();
  const float col_w = std::min(880.0F, area.w - 64.0F);
  const float col_x = area.x + (area.w - col_w) * 0.5F;
  float y = area.y + 34.0F;

  dl.companion(CompanionId::moss, col_x + 24.0F, y + 22.0F, 22.0F, CompanionMood::idle, time_sec_, animate_companions_,
               pet_bounces_[static_cast<std::size_t>(CompanionId::moss)]);
  atlas.draw_line(dl, col_x + 60.0F, y + 2.0F, "长期记忆", FontRole::display, 26.0F, th.text);
  atlas.draw_line(dl, col_x + 60.0F, y + 34.0F, "小苔帮你沉淀在本机 SQLite FTS5，跨会话自动记住你的偏好与工作脉络",
                  FontRole::ui, 13.0F, th.subtext);
  y += 66.0F;

  // Quick-add memory card
  const Rect add_card{col_x, y, col_w, 72.0F};
  dl.shadow(add_card, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.6F));
  dl.rect(add_card, 14.0F, th.surface, th.line, 1.0F);
  draw_text_input(dl, atlas, Rect{add_card.x + 14.0F, add_card.y + 16.0F, 200.0F, 40.0F}, InputField::memory_title,
                  mem_title_input_, "记忆标题（如：回复偏好）", 13.5F);
  draw_text_input(dl, atlas, Rect{add_card.x + 224.0F, add_card.y + 16.0F, add_card.w - 344.0F, 40.0F},
                  InputField::memory_body, mem_body_input_, "输入要记住的内容，回车保存…", 13.5F);
  draw_button(dl, atlas, Rect{add_card.x + add_card.w - 108.0F, add_card.y + 16.0F, 94.0F, 40.0F}, "保存记忆", true,
              [this]() {
                if (!mem_title_input_.empty() && !mem_body_input_.empty()) {
                  bridge_.add_memory_note(mem_title_input_, mem_body_input_, "personal");
                  mem_title_input_.clear();
                  mem_body_input_.clear();
                }
              });
  y += 92.0F;

  if (snap.notes.empty()) {
    const Rect empty{col_x, y, col_w, 120.0F};
    dl.rect(empty, 14.0F, th.surface, th.line_soft, 1.0F);
    const std::string_view msg = "还没有保存长期记忆，在上方新增一条，或在对话中直接说「记住：…」";
    const float mw = atlas.measure_line(msg, FontRole::ui, 13.5F);
    atlas.draw_line(dl, empty.x + (empty.w - mw) * 0.5F, empty.y + 50.0F, msg, FontRole::ui, 13.5F, th.muted);
    return;
  }

  const float gap = 16.0F;
  const float cw = (col_w - gap) * 0.5F;
  const float ch = 128.0F;
  for (std::size_t i = 0; i < snap.notes.size(); ++i) {
    const auto& note = snap.notes[i];
    const float cx = col_x + static_cast<float>(i % 2U) * (cw + gap);
    const float cy = y + static_cast<float>(i / 2U) * (ch + gap);
    const Rect card{cx, cy, cw, ch};
    dl.shadow(card, 14.0F, 8.0F, 2.0F, th.shadow.with_alpha(0.65F));
    dl.rect(card, 14.0F, th.raised, th.line, 1.0F);

    const bool is_shared = note.scope == "shared";
    const Rect pill{card.x + 16.0F, card.y + 14.0F, 56.0F, 22.0F};
    dl.rect(pill, 11.0F, is_shared ? th.blue_soft : th.green_soft);
    atlas.draw_line(dl, pill.x + 12.0F, pill.y + 4.0F, is_shared ? "工作区" : "个人", FontRole::ui_bold, 11.0F,
                    is_shared ? th.blue : th.green);

    atlas.draw_line(dl, card.x + 82.0F, card.y + 16.0F, note.title, FontRole::ui_bold, 14.0F, th.text, cw - 126.0F,
                    true);

    const Rect del{card.x + card.w - 36.0F, card.y + 12.0F, 24.0F, 24.0F};
    if (del.contains(mouse_x_, mouse_y_)) {
      dl.rect(del, 6.0F, th.red_soft);
    }
    dl.icon(IconKind::trash, del.x + 12.0F, del.y + 12.0F, 12.5F, th.muted);
    register_hit(del, [this, id = note.id, sc = note.scope]() { bridge_.delete_memory_note(id, sc); });

    dl.push_clip(Rect{card.x + 16.0F, card.y + 46.0F, card.w - 32.0F, card.h - 58.0F});
    atlas.draw_wrapped(dl, card.x + 16.0F, card.y + 46.0F, note.body, FontRole::ui, 13.0F, th.subtext, card.w - 32.0F);
    dl.pop_clip();
  }
}

void DesktopApp::render_reminders_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area) {
  const auto& th = theme();
  const float col_w = std::min(780.0F, area.w - 64.0F);
  const float col_x = area.x + (area.w - col_w) * 0.5F;
  float y = area.y + 34.0F;

  dl.companion(CompanionId::pip, col_x + 24.0F, y + 22.0F, 22.0F, CompanionMood::idle, time_sec_, animate_companions_,
               pet_bounces_[static_cast<std::size_t>(CompanionId::pip)]);
  atlas.draw_line(dl, col_x + 60.0F, y + 2.0F, "轻量提醒", FontRole::display, 26.0F, th.text);
  atlas.draw_line(dl, col_x + 60.0F, y + 34.0F, "啾啾帮你守着每个时间节点，数据持久化在本机 personal.db",
                  FontRole::ui, 13.0F, th.subtext);
  y += 66.0F;

  // Quick-add reminder bar
  const Rect add_card{col_x, y, col_w, 72.0F};
  dl.shadow(add_card, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.6F));
  dl.rect(add_card, 14.0F, th.surface, th.line, 1.0F);

  draw_text_input(dl, atlas, Rect{add_card.x + 14.0F, add_card.y + 16.0F, add_card.w - 324.0F, 40.0F},
                  InputField::reminder_title, rem_title_input_, "添加一条提醒事项…", 13.5F);

  const std::array<std::string_view, 3> presets{"30分后", "2小时", "明天"};
  float px = add_card.x + add_card.w - 298.0F;
  for (int i = 0; i < 3; ++i) {
    const Rect pr{px, add_card.y + 20.0F, 58.0F, 32.0F};
    const bool sel = rem_delay_index_ == i;
    dl.rect(pr, 8.0F, sel ? th.selected : th.raised, sel ? th.accent_line : th.line, 1.0F);
    const float lw = atlas.measure_line(presets[static_cast<std::size_t>(i)], FontRole::ui_bold, 11.5F);
    atlas.draw_line(dl, pr.x + (pr.w - lw) * 0.5F, pr.y + 8.0F, presets[static_cast<std::size_t>(i)], FontRole::ui_bold,
                    11.5F, sel ? th.selected_text : th.subtext);
    register_hit(pr, [this, i]() { rem_delay_index_ = i; });
    px += 64.0F;
  }

  draw_button(dl, atlas, Rect{add_card.x + add_card.w - 98.0F, add_card.y + 16.0F, 84.0F, 40.0F}, "添加", true,
              [this]() {
                if (!rem_title_input_.empty()) {
                  constexpr std::array<std::int64_t, 3> kOffsetsMs{30LL * 60 * 1000, 120LL * 60 * 1000,
                                                                   24LL * 3600 * 1000};
                  bridge_.add_reminder(rem_title_input_,
                                       now_ms() + kOffsetsMs[static_cast<std::size_t>(rem_delay_index_)]);
                  rem_title_input_.clear();
                }
              });
  y += 92.0F;

  if (snap.reminders.empty()) {
    const Rect empty{col_x, y, col_w, 120.0F};
    dl.rect(empty, 14.0F, th.surface, th.line_soft, 1.0F);
    const std::string_view msg = "暂时没有待办提醒，享受轻松的专注时光吧";
    const float mw = atlas.measure_line(msg, FontRole::ui, 13.5F);
    atlas.draw_line(dl, empty.x + (empty.w - mw) * 0.5F, empty.y + 50.0F, msg, FontRole::ui, 13.5F, th.muted);
    return;
  }

  for (const auto& rem : snap.reminders) {
    const Rect row{col_x, y, col_w, 60.0F};
    dl.shadow(row, 13.0F, 8.0F, 2.0F, th.shadow.with_alpha(0.55F));
    dl.rect(row, 13.0F, th.raised, th.line, 1.0F);

    const Rect chk{row.x + 18.0F, row.y + 19.0F, 22.0F, 22.0F};
    dl.rect(chk, 7.0F, rem.completed ? th.green : th.surface, rem.completed ? th.green : th.line_strong, 1.5F);
    if (rem.completed) {
      dl.icon(IconKind::check, chk.x + 11.0F, chk.y + 11.0F, 12.0F, th.on_accent);
    }
    register_hit(chk, [this, id = rem.id, done = !rem.completed]() { bridge_.toggle_reminder(id, done); });

    atlas.draw_line(dl, row.x + 54.0F, row.y + 12.0F, rem.title, FontRole::ui_bold, 14.0F,
                    rem.completed ? th.muted : th.text, row.w - 190.0F, true);
    atlas.draw_line(dl, row.x + 54.0F, row.y + 35.0F, format_due_label(rem.due_at_ms), FontRole::ui, 11.5F, th.muted);

    const Rect del{row.x + row.w - 40.0F, row.y + 18.0F, 24.0F, 24.0F};
    if (del.contains(mouse_x_, mouse_y_)) {
      dl.rect(del, 6.0F, th.red_soft);
    }
    dl.icon(IconKind::trash, del.x + 12.0F, del.y + 12.0F, 12.5F, th.muted);
    register_hit(del, [this, id = rem.id]() { bridge_.delete_reminder(id); });

    y += 72.0F;
  }
}

void DesktopApp::render_settings_view(DrawList& dl, FontAtlas& atlas, const AgentSnapshot& snap, Rect area) {
  const auto& th = theme();
  const float col_w = std::min(760.0F, area.w - 64.0F);
  const float col_x = area.x + (area.w - col_w) * 0.5F;
  float y = area.y + 34.0F;

  atlas.draw_line(dl, col_x, y, "偏好设置", FontRole::display, 26.0F, th.text);
  y += 52.0F;

  // Card 1: Appearance & Companion Motion
  const Rect c1{col_x, y, col_w, 148.0F};
  dl.shadow(c1, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.55F));
  dl.rect(c1, 14.0F, th.raised, th.line, 1.0F);
  atlas.draw_line(dl, c1.x + 22.0F, c1.y + 18.0F, "外观与伙伴动画", FontRole::ui_bold, 15.0F, th.text);
  atlas.draw_line(dl, c1.x + 22.0F, c1.y + 58.0F, "界面主题", FontRole::ui, 13.5F, th.subtext);
  draw_button(dl, atlas, Rect{c1.x + c1.w - 208.0F, c1.y + 48.0F, 88.0F, 32.0F}, "暖纸浅色", !dark_mode_,
              [this]() { dark_mode_ = false; });
  draw_button(dl, atlas, Rect{c1.x + c1.w - 110.0F, c1.y + 48.0F, 88.0F, 32.0F}, "松木深色", dark_mode_,
              [this]() { dark_mode_ = true; });

  dl.rect(Rect{c1.x + 22.0F, c1.y + 92.0F, c1.w - 44.0F, 1.0F}, 0.0F, th.line_soft);
  atlas.draw_line(dl, c1.x + 22.0F, c1.y + 108.0F, "伙伴呼吸与眨眼微动效", FontRole::ui, 13.5F, th.subtext);
  draw_button(dl, atlas, Rect{c1.x + c1.w - 110.0F, c1.y + 100.0F, 88.0F, 32.0F},
              animate_companions_ ? "已开启" : "已关闭", animate_companions_,
              [this]() { animate_companions_ = !animate_companions_; });
  y += 168.0F;

  // Card 2: Permission policy
  const Rect c2{col_x, y, col_w, 118.0F};
  dl.shadow(c2, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.55F));
  dl.rect(c2, 14.0F, th.raised, th.line, 1.0F);
  atlas.draw_line(dl, c2.x + 22.0F, c2.y + 18.0F, "安全与操作确认边界", FontRole::ui_bold, 15.0F, th.text);
  atlas.draw_line(dl, c2.x + 22.0F, c2.y + 44.0F, "写入文件或执行命令时，石墩按此策略决定是否拦截确认", FontRole::ui,
                  12.5F, th.muted);
  draw_button(dl, atlas, Rect{c2.x + 22.0F, c2.y + 72.0F, 128.0F, 32.0F}, "每次确认（推荐）",
              snap.permission_policy == "ask", [this]() { bridge_.set_permission_policy("ask"); });
  draw_button(dl, atlas, Rect{c2.x + 162.0F, c2.y + 72.0F, 106.0F, 32.0F}, "自动允许",
              snap.permission_policy == "allow", [this]() { bridge_.set_permission_policy("allow"); });
  draw_button(dl, atlas, Rect{c2.x + 280.0F, c2.y + 72.0F, 106.0F, 32.0F}, "严格只读",
              snap.permission_policy == "deny", [this]() { bridge_.set_permission_policy("deny"); });
  y += 138.0F;

  // Card 3: Native Runtime Info
  const Rect c3{col_x, y, col_w, 168.0F};
  dl.shadow(c3, 14.0F, 10.0F, 2.0F, th.shadow.with_alpha(0.55F));
  dl.rect(c3, 14.0F, th.raised, th.line, 1.0F);
  atlas.draw_line(dl, c3.x + 22.0F, c3.y + 18.0F, "本地运行环境", FontRole::ui_bold, 15.0F, th.text);

  const std::array<std::pair<std::string_view, std::string>, 4> rows{{
      {"图形引擎", std::format("Pure Vulkan 1.0 SDF ({})", gpu_name_)},
      {"模型配置", snap.model_name},
      {"当前工作区", snap.workspace_path},
      {"本地状态库", snap.state_dir},
  }};
  float ry = c3.y + 52.0F;
  for (const auto& [k, v] : rows) {
    atlas.draw_line(dl, c3.x + 22.0F, ry, k, FontRole::ui_bold, 12.5F, th.muted);
    atlas.draw_line(dl, c3.x + 124.0F, ry, v, FontRole::mono, 12.0F, th.text, c3.w - 146.0F, true);
    ry += 27.0F;
  }
}

}  // namespace orangutan::vkui
