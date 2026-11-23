#include "font_atlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace orangutan::vkui {
namespace {

[[nodiscard]] std::uint64_t make_glyph_key(FontRole role, std::uint32_t px, char32_t cp) noexcept {
  return (static_cast<std::uint64_t>(role) << 56U) |
         ((static_cast<std::uint64_t>(px) & 0xFFFFULL) << 32U) |
         static_cast<std::uint64_t>(cp);
}

[[nodiscard]] std::size_t decode_utf8(std::string_view s, std::size_t pos, char32_t& out) noexcept {
  const auto b0 = static_cast<std::uint8_t>(s[pos]);
  if (b0 < 0x80U) {
    out = b0;
    return 1;
  }
  if ((b0 & 0xE0U) == 0xC0U && pos + 1 < s.size()) {
    out = ((b0 & 0x1FU) << 6U) | (static_cast<std::uint8_t>(s[pos + 1]) & 0x3FU);
    return 2;
  }
  if ((b0 & 0xF0U) == 0xE0U && pos + 2 < s.size()) {
    out = ((b0 & 0x0FU) << 12U) | ((static_cast<std::uint8_t>(s[pos + 1]) & 0x3FU) << 6U) |
          (static_cast<std::uint8_t>(s[pos + 2]) & 0x3FU);
    return 3;
  }
  if ((b0 & 0xF8U) == 0xF0U && pos + 3 < s.size()) {
    out = ((b0 & 0x07U) << 18U) | ((static_cast<std::uint8_t>(s[pos + 1]) & 0x3FU) << 12U) |
          ((static_cast<std::uint8_t>(s[pos + 2]) & 0x3FU) << 6U) |
          (static_cast<std::uint8_t>(s[pos + 3]) & 0x3FU);
    return 4;
  }
  out = U'?';
  return 1;
}

[[nodiscard]] bool is_cjk_or_wide(char32_t cp) noexcept {
  return (cp >= 0x2E80U && cp <= 0x9FFFU) || (cp >= 0xF900U && cp <= 0xFAFFU) ||
         (cp >= 0xFF00U && cp <= 0xFFEFU) || (cp >= 0x3000U && cp <= 0x303FU);
}

[[nodiscard]] std::string query_fontconfig(const char* pattern_str) {
  std::string found;
  if (FcInit() == FcFalse) {
    return found;
  }
  FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(pattern_str));
  if (pat == nullptr) {
    return found;
  }
  FcConfigSubstitute(nullptr, pat, FcMatchPattern);
  FcDefaultSubstitute(pat);
  FcResult res = FcResultNoMatch;
  FcPattern* match = FcFontMatch(nullptr, pat, &res);
  if (match != nullptr) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file != nullptr) {
      found = reinterpret_cast<const char*>(file);
    }
    FcPatternDestroy(match);
  }
  FcPatternDestroy(pat);
  return found;
}

[[nodiscard]] std::string pick_first_existing(std::initializer_list<const char*> candidates, const char* fc_fallback) {
  for (const char* path : candidates) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return query_fontconfig(fc_fallback);
}

}  // namespace

struct FontAtlas::Impl {
  FT_Library ft{nullptr};
  FT_Face ui_primary{nullptr};
  FT_Face ui_bold{nullptr};
  FT_Face display{nullptr};
  FT_Face mono{nullptr};
  FT_Face cjk_fallback{nullptr};

  void shutdown() {
    auto kill = [](FT_Face& f) {
      if (f != nullptr) {
        FT_Done_Face(f);
        f = nullptr;
      }
    };
    kill(ui_primary);
    kill(ui_bold);
    kill(display);
    kill(mono);
    kill(cjk_fallback);
    if (ft != nullptr) {
      FT_Done_FreeType(ft);
      ft = nullptr;
    }
  }
};

FontAtlas::FontAtlas() : impl_{new Impl{}}, pixels_(static_cast<std::size_t>(kAtlasSize) * kAtlasSize, 0U) {}

FontAtlas::~FontAtlas() {
  if (impl_ != nullptr) {
    impl_->shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool FontAtlas::init(float dpr) {
  dpr_ = std::max(dpr, 0.5F);
  if (FT_Init_FreeType(&impl_->ft) != 0) {
    return false;
  }

  const auto load_face = [&](const std::string& path, FT_Face& out) {
    if (path.empty()) {
      return;
    }
    if (FT_New_Face(impl_->ft, path.c_str(), 0, &out) != 0) {
      out = nullptr;
    }
  };

  const std::string cjk_reg = pick_first_existing(
      {
          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
          "/usr/share/fonts/harmonyos-sans/HarmonyOS_Sans_SC.ttf",
          "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Regular.otf",
      },
      "Noto Sans CJK SC:style=Regular");

  const std::string cjk_bold = pick_first_existing(
      {
          "/usr/share/fonts/noto-cjk/NotoSansCJK-Medium.ttc",
          "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
          "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Medium.otf",
          "/usr/share/fonts/harmonyos-sans/HarmonyOS_Sans_SC.ttf",
      },
      "Noto Sans CJK SC:style=Bold");

  const std::string display_path = pick_first_existing(
      {
          "/usr/share/fonts/noto-cjk/NotoSerifCJK-SemiBold.ttc",
          "/usr/share/fonts/noto-cjk/NotoSerifCJK-Bold.ttc",
          "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
      },
      "Noto Serif CJK SC:style=SemiBold");

  const std::string mono_path = pick_first_existing(
      {
          "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
          "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
          "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
      },
      "monospace");

  load_face(cjk_reg, impl_->ui_primary);
  load_face(cjk_bold, impl_->ui_bold);
  load_face(display_path, impl_->display);
  load_face(mono_path, impl_->mono);
  load_face(cjk_reg, impl_->cjk_fallback);

  if (impl_->ui_primary == nullptr) {
    const auto fallback = query_fontconfig("sans-serif");
    load_face(fallback, impl_->ui_primary);
  }
  return impl_->ui_primary != nullptr;
}

void FontAtlas::set_dpr(float dpr) {
  const float clamped = std::max(dpr, 0.5F);
  if (std::abs(clamped - dpr_) < 0.05F) {
    return;
  }
  dpr_ = clamped;
  cache_.clear();
  std::ranges::fill(pixels_, std::uint8_t{0});
  pen_x_ = 2U;
  pen_y_ = 2U;
  row_h_ = 0U;
  dirty_ = true;
}

const GlyphEntry* FontAtlas::get_glyph(FontRole role, std::uint32_t px_size, char32_t cp) {
  const std::uint64_t key = make_glyph_key(role, px_size, cp);
  if (const auto it = cache_.find(key); it != cache_.end()) {
    return &it->second;
  }

  FT_Face primary = impl_->ui_primary;
  if (role == FontRole::ui_bold && impl_->ui_bold != nullptr) {
    primary = impl_->ui_bold;
  } else if (role == FontRole::display && impl_->display != nullptr) {
    primary = impl_->display;
  } else if (role == FontRole::mono && impl_->mono != nullptr) {
    primary = impl_->mono;
  }

  FT_Face chosen = primary;
  FT_UInt glyph_index = (chosen != nullptr) ? FT_Get_Char_Index(chosen, static_cast<FT_ULong>(cp)) : 0U;
  if (glyph_index == 0U && impl_->cjk_fallback != nullptr && impl_->cjk_fallback != chosen) {
    const FT_UInt fb_index = FT_Get_Char_Index(impl_->cjk_fallback, static_cast<FT_ULong>(cp));
    if (fb_index != 0U) {
      chosen = impl_->cjk_fallback;
      glyph_index = fb_index;
    }
  }
  if (chosen == nullptr) {
    return nullptr;
  }

  FT_Set_Pixel_Sizes(chosen, 0U, px_size);
  if (FT_Load_Glyph(chosen, glyph_index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0) {
    return nullptr;
  }

  const FT_GlyphSlot slot = chosen->glyph;
  const FT_Bitmap& bmp = slot->bitmap;
  const std::uint32_t gw = bmp.width;
  const std::uint32_t gh = bmp.rows;

  GlyphEntry entry{};
  entry.bearing_x = static_cast<float>(slot->bitmap_left) / dpr_;
  entry.bearing_y = static_cast<float>(slot->bitmap_top) / dpr_;
  entry.width = static_cast<float>(gw) / dpr_;
  entry.height = static_cast<float>(gh) / dpr_;
  entry.advance = static_cast<float>(slot->advance.x >> 6) / dpr_;

  if (gw > 0U && gh > 0U) {
    if (pen_x_ + gw + 2U >= kAtlasSize) {
      pen_x_ = 2U;
      pen_y_ += row_h_ + 2U;
      row_h_ = 0U;
    }
    if (pen_y_ + gh + 2U >= kAtlasSize) {
      // Atlas full: reset cleanly and re-pack on demand.
      cache_.clear();
      std::ranges::fill(pixels_, std::uint8_t{0});
      pen_x_ = 2U;
      pen_y_ = 2U;
      row_h_ = 0U;
    }

    for (std::uint32_t row = 0U; row < gh; ++row) {
      const std::uint8_t* src = bmp.buffer + static_cast<std::size_t>(row) * static_cast<std::size_t>(bmp.pitch);
      std::uint8_t* dst = pixels_.data() + static_cast<std::size_t>(pen_y_ + row) * kAtlasSize + pen_x_;
      std::memcpy(dst, src, gw);
    }

    const float inv = 1.0F / static_cast<float>(kAtlasSize);
    entry.u0 = static_cast<float>(pen_x_) * inv;
    entry.v0 = static_cast<float>(pen_y_) * inv;
    entry.u1 = static_cast<float>(pen_x_ + gw) * inv;
    entry.v1 = static_cast<float>(pen_y_ + gh) * inv;

    pen_x_ += gw + 2U;
    row_h_ = std::max(row_h_, gh);
    dirty_ = true;
  }

  auto [it, _] = cache_.emplace(key, entry);
  return &it->second;
}

float FontAtlas::measure_line(std::string_view utf8, FontRole role, float size_pt) {
  const auto px = static_cast<std::uint32_t>(std::max(8.0F, std::round(size_pt * dpr_)));
  float w = 0.0F;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    char32_t cp = 0;
    pos += decode_utf8(utf8, pos, cp);
    if (cp == U'\n' || cp == U'\r') {
      continue;
    }
    if (const auto* g = get_glyph(role, px, cp)) {
      w += g->advance;
    }
  }
  return w;
}

std::vector<std::string> FontAtlas::wrap_lines(std::string_view utf8, FontRole role, float size_pt, float max_width) {
  std::vector<std::string> lines;
  if (utf8.empty()) {
    lines.emplace_back();
    return lines;
  }
  const auto px = static_cast<std::uint32_t>(std::max(8.0F, std::round(size_pt * dpr_)));
  const float limit = max_width > 20.0F ? max_width : 100000.0F;

  std::string current;
  float cur_w = 0.0F;
  std::size_t pos = 0;

  while (pos < utf8.size()) {
    char32_t cp = 0;
    const std::size_t len = decode_utf8(utf8, pos, cp);
    if (cp == U'\r') {
      pos += len;
      continue;
    }
    if (cp == U'\n') {
      lines.push_back(std::move(current));
      current.clear();
      cur_w = 0.0F;
      pos += len;
      continue;
    }
    const auto* g = get_glyph(role, px, cp);
    const float adv = g != nullptr ? g->advance : size_pt * 0.5F;

    if (!current.empty() && cur_w + adv > limit) {
      // Soft break at last space for Latin runs; CJK breaks cleanly at character boundaries.
      if (!is_cjk_or_wide(cp)) {
        const auto sp = current.rfind(' ');
        if (sp != std::string::npos && sp > current.size() / 2) {
          std::string next = current.substr(sp + 1);
          current.resize(sp);
          lines.push_back(std::move(current));
          current = std::move(next);
          cur_w = measure_line(current, role, size_pt);
        } else {
          lines.push_back(std::move(current));
          current.clear();
          cur_w = 0.0F;
        }
      } else {
        lines.push_back(std::move(current));
        current.clear();
        cur_w = 0.0F;
      }
    }

    current.append(utf8.substr(pos, len));
    cur_w += adv;
    pos += len;
  }
  lines.push_back(std::move(current));
  return lines;
}

TextBlockMetrics FontAtlas::measure_wrapped(std::string_view utf8, FontRole role, float size_pt, float max_width) {
  const auto lines = wrap_lines(utf8, role, size_pt, max_width);
  float max_w = 0.0F;
  for (const auto& l : lines) {
    max_w = std::max(max_w, measure_line(l, role, size_pt));
  }
  return TextBlockMetrics{
      .width = max_w,
      .height = static_cast<float>(lines.size()) * line_height(size_pt),
      .line_count = lines.size(),
  };
}

float FontAtlas::draw_line(DrawList& dl, float x, float y_top, std::string_view utf8, FontRole role, float size_pt,
                           Color color, float max_width, bool ellipsis) {
  const auto px = static_cast<std::uint32_t>(std::max(8.0F, std::round(size_pt * dpr_)));
  const float baseline = y_top + size_pt * 1.05F;
  const float ellipsis_w = (max_width > 0.0F && ellipsis) ? measure_line("…", role, size_pt) : 0.0F;
  const float gamma = (color.r + color.g + color.b) > 1.5F ? 0.92F : 0.84F;

  float cursor_x = x;
  std::size_t pos = 0;
  while (pos < utf8.size()) {
    char32_t cp = 0;
    const std::size_t len = decode_utf8(utf8, pos, cp);
    pos += len;
    if (cp == U'\n' || cp == U'\r') {
      continue;
    }
    const auto* g = get_glyph(role, px, cp);
    if (g == nullptr) {
      continue;
    }
    if (max_width > 0.0F && (cursor_x - x) + g->advance + ellipsis_w > max_width) {
      if (ellipsis) {
        if (const auto* eg = get_glyph(role, px, U'\u2026')) {
          if (eg->width > 0.0F && eg->height > 0.0F) {
            dl.glyph(Rect{std::round(cursor_x + eg->bearing_x), std::round(baseline - eg->bearing_y), eg->width,
                          eg->height},
                     eg->u0, eg->v0, eg->u1, eg->v1, color, gamma);
          }
          cursor_x += eg->advance;
        }
      }
      break;
    }
    if (g->width > 0.0F && g->height > 0.0F) {
      dl.glyph(Rect{std::round(cursor_x + g->bearing_x), std::round(baseline - g->bearing_y), g->width, g->height},
               g->u0, g->v0, g->u1, g->v1, color, gamma);
    }
    cursor_x += g->advance;
  }
  return cursor_x - x;
}

TextBlockMetrics FontAtlas::draw_wrapped(DrawList& dl, float x, float y_top, std::string_view utf8, FontRole role,
                                         float size_pt, Color color, float max_width) {
  const auto lines = wrap_lines(utf8, role, size_pt, max_width);
  const float lh = line_height(size_pt);
  float max_w = 0.0F;
  float y = y_top;
  for (const auto& line : lines) {
    const float lw = draw_line(dl, x, y, line, role, size_pt, color);
    max_w = std::max(max_w, lw);
    y += lh;
  }
  return TextBlockMetrics{
      .width = max_w,
      .height = static_cast<float>(lines.size()) * lh,
      .line_count = lines.size(),
  };
}

}  // namespace orangutan::vkui
