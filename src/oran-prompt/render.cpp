#include <oran/prompt/render.hpp>

#include <algorithm>
#include <array>

namespace orangutan::prompt {
namespace {

constexpr std::uint64_t FNV_OFFSET = 14695981039346656037ull;
constexpr std::uint64_t FNV_PRIME = 1099511628211ull;

[[nodiscard]] std::uint64_t hash_append(std::uint64_t hash, std::string_view bytes) noexcept {
  for (const char c : bytes) {
    hash ^= static_cast<unsigned char>(c);
    hash *= FNV_PRIME;
  }
  return hash;
}

[[nodiscard]] std::uint64_t hash_append_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  std::array<char, 8> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>((value >> (i * 8)) & 0xFFu);
  }
  return hash_append(hash, std::string_view{bytes.data(), bytes.size()});
}

[[nodiscard]] std::uint64_t hash_field(std::uint64_t hash, std::string_view bytes) noexcept {
  return hash_append(hash_append_u64(hash, bytes.size()), bytes);
}

[[nodiscard]] std::uint64_t hash_tools(std::span<const core::ToolDef> tools) noexcept {
  auto hash = hash_append_u64(FNV_OFFSET, tools.size());
  for (const auto& def : tools) {
    hash = hash_field(hash, def.name);
    hash = hash_field(hash, def.description);
    hash = hash_field(hash, def.input_schema_json);
  }
  return hash;
}

}  // namespace

RenderedPrompt render(RenderInputs inputs) {
  const std::array sections{
      inputs.system_preamble,
      inputs.skills_catalog,
      inputs.memory_framing,
      inputs.per_agent_overlay,
  };
  const auto text_bytes =
      std::ranges::fold_left(sections, std::size_t{0}, [](auto bytes, auto text) { return bytes + text.size(); });
  RenderedPrompt rendered;
  rendered.system_prompt.reserve(text_bytes + sections.size() - 1);
  for (const auto text : sections) {
    if (!text.empty()) {
      if (!rendered.system_prompt.empty()) {
        rendered.system_prompt.push_back('\n');
      }
      rendered.system_prompt.append(text);
    }
  }
  rendered.tool_catalog_hash = hash_tools(inputs.tools);
  rendered.prefix_hash = hash_append_u64(hash_field(FNV_OFFSET, rendered.system_prompt), rendered.tool_catalog_hash);
  rendered.prefix_bytes =
      std::ranges::fold_left(inputs.tools, rendered.system_prompt.size(), [](auto bytes, const auto& def) {
        return bytes + def.name.size() + def.description.size() + def.input_schema_json.size();
      });
  return rendered;
}

}  // namespace orangutan::prompt
