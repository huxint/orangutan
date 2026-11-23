#pragma once

#include <cstdint>

namespace orangutan::vkui {

struct Color {
  float r{0.0F};
  float g{0.0F};
  float b{0.0F};
  float a{1.0F};

  [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
    return Color{r, g, b, alpha};
  }
};

[[nodiscard]] constexpr Color hex(std::uint32_t rgb, float alpha = 1.0F) noexcept {
  return Color{
      static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F,
      static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
      static_cast<float>(rgb & 0xFFU) / 255.0F,
      alpha,
  };
}

struct Theme {
  bool dark{false};

  Color bg{};
  Color sidebar{};
  Color sidebar_hover{};
  Color surface{};
  Color raised{};
  Color panel{};
  Color hover{};

  Color text{};
  Color subtext{};
  Color muted{};

  Color line{};
  Color line_soft{};
  Color line_strong{};

  Color selected{};
  Color selected_text{};

  Color accent{};
  Color accent_hover{};
  Color accent_soft{};
  Color accent_line{};
  Color on_accent{};

  Color green{};
  Color green_soft{};
  Color blue{};
  Color blue_soft{};
  Color red{};
  Color red_soft{};
  Color amber{};
  Color amber_soft{};

  Color shadow{};
};

[[nodiscard]] inline constexpr Theme light_theme() noexcept {
  return Theme{
      .dark = false,
      .bg = hex(0xF6F2EA),
      .sidebar = hex(0xEDE7DB),
      .sidebar_hover = hex(0xE4DCCD),
      .surface = hex(0xFBF8F2),
      .raised = hex(0xFFFDF8),
      .panel = hex(0xF1EBE0),
      .hover = hex(0xE9E2D4),
      .text = hex(0x292118),
      .subtext = hex(0x5B4E3F),
      .muted = hex(0x786A57),
      .line = hex(0xDCD3C4),
      .line_soft = hex(0xE7E0D2),
      .line_strong = hex(0xB9AE9C),
      .selected = hex(0xECDFD2),
      .selected_text = hex(0x7A3411),
      .accent = hex(0xB24A1D),
      .accent_hover = hex(0x9C3F17),
      .accent_soft = hex(0xF2E3D7),
      .accent_line = hex(0xDDBCA6),
      .on_accent = hex(0xFFFFFF),
      .green = hex(0x3D6B4A),
      .green_soft = hex(0xE6EDE2),
      .blue = hex(0x476B83),
      .blue_soft = hex(0xE5EDF2),
      .red = hex(0xA3442E),
      .red_soft = hex(0xF5E5DE),
      .amber = hex(0xB37620),
      .amber_soft = hex(0xF6ECD9),
      .shadow = hex(0x292118, 0.10F),
  };
}

[[nodiscard]] inline constexpr Theme dark_theme() noexcept {
  return Theme{
      .dark = true,
      .bg = hex(0x1E1914),
      .sidebar = hex(0x17130E),
      .sidebar_hover = hex(0x251F18),
      .surface = hex(0x272018),
      .raised = hex(0x30281F),
      .panel = hex(0x231D16),
      .hover = hex(0x312920),
      .text = hex(0xF1EADE),
      .subtext = hex(0xD4CBB6),
      .muted = hex(0x9E917B),
      .line = hex(0x3D3428),
      .line_soft = hex(0x2F271F),
      .line_strong = hex(0x5B4F3E),
      .selected = hex(0x3A2A1F),
      .selected_text = hex(0xF0C0A2),
      .accent = hex(0xE08A5D),
      .accent_hover = hex(0xEBA27C),
      .accent_soft = hex(0x3A2A1E),
      .accent_line = hex(0x5C3D29),
      .on_accent = hex(0x1E1914),
      .green = hex(0x8FB897),
      .green_soft = hex(0x233024),
      .blue = hex(0x8EB0C7),
      .blue_soft = hex(0x212D36),
      .red = hex(0xE29B87),
      .red_soft = hex(0x3A251F),
      .amber = hex(0xE5B263),
      .amber_soft = hex(0x362C1C),
      .shadow = hex(0x000000, 0.34F),
  };
}

enum class CompanionId : std::uint8_t {
  oran = 0,
  scout,
  moss,
  fern,
  pebble,
  pip,
};

enum class CompanionMood : std::uint8_t {
  idle = 0,
  thinking,
  working,
  guarding,
  celebrating,
};

}  // namespace orangutan::vkui
