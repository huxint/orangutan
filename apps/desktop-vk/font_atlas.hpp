#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "primitives.hpp"

namespace orangutan::vkui {

enum class FontRole : std::uint8_t {
  ui = 0,
  ui_bold,
  display,
  mono,
};

struct GlyphEntry {
  float u0{0.0F};
  float v0{0.0F};
  float u1{0.0F};
  float v1{0.0F};
  float bearing_x{0.0F};
  float bearing_y{0.0F};
  float width{0.0F};
  float height{0.0F};
  float advance{0.0F};
};

struct TextBlockMetrics {
  float width{0.0F};
  float height{0.0F};
  std::size_t line_count{0};
};

class FontAtlas {
public:
  static constexpr std::uint32_t kAtlasSize = 2048U;

  FontAtlas();
  ~FontAtlas();

  FontAtlas(const FontAtlas&) = delete;
  FontAtlas& operator=(const FontAtlas&) = delete;

  bool init(float dpr = 1.0F);
  void set_dpr(float dpr);

  [[nodiscard]] float measure_line(std::string_view utf8, FontRole role, float size_pt);
  [[nodiscard]] float line_height(float size_pt) const noexcept {
    return size_pt * 1.48F;
  }

  [[nodiscard]] TextBlockMetrics measure_wrapped(std::string_view utf8, FontRole role, float size_pt, float max_width);
  float draw_line(DrawList& dl, float x, float y_top, std::string_view utf8, FontRole role, float size_pt,
                  Color color, float max_width = 0.0F, bool ellipsis = false);
  TextBlockMetrics draw_wrapped(DrawList& dl, float x, float y_top, std::string_view utf8, FontRole role,
                                float size_pt, Color color, float max_width);

  [[nodiscard]] bool dirty() const noexcept {
    return dirty_;
  }
  void clear_dirty() noexcept {
    dirty_ = false;
  }
  [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept {
    return pixels_;
  }

private:
  struct Impl;
  Impl* impl_{nullptr};

  [[nodiscard]] const GlyphEntry* get_glyph(FontRole role, std::uint32_t px_size, char32_t cp);
  [[nodiscard]] std::vector<std::string> wrap_lines(std::string_view utf8, FontRole role, float size_pt,
                                                    float max_width);

  float dpr_{1.0F};
  std::uint32_t pen_x_{2U};
  std::uint32_t pen_y_{2U};
  std::uint32_t row_h_{0U};
  bool dirty_{true};
  std::vector<std::uint8_t> pixels_;
  std::unordered_map<std::uint64_t, GlyphEntry> cache_;
};

}  // namespace orangutan::vkui
