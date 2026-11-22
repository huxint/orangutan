#include <oran/tool/catalog.hpp>

#include <algorithm>
#include <expected>
#include <ranges>

#include <oran/core/error.hpp>

namespace orangutan::tool {

core::Result<std::vector<core::ToolDef>> select_tools(std::span<const core::ToolDef> catalog,
                                                      std::optional<std::span<const std::string>> names) {
  if (names) {
    for (const auto& name : *names) {
      if (!std::ranges::contains(catalog, name, &core::ToolDef::name)) {
        return std::unexpected(core::Error::not_found("active tool is not registered").with("tool", name));
      }
    }
  }

  auto selected = catalog | std::views::filter([names](const core::ToolDef& def) {
                    return !names || std::ranges::contains(*names, def.name);
                  }) |
                  std::ranges::to<std::vector>();
  std::ranges::sort(selected, {}, &core::ToolDef::name);
  const auto duplicate = std::ranges::adjacent_find(selected, {}, &core::ToolDef::name);
  if (duplicate != selected.end()) {
    return std::unexpected(
        core::Error::invalid_argument("tool catalogue contains duplicate names").with("tool", duplicate->name));
  }
  return selected;
}

}  // namespace orangutan::tool
