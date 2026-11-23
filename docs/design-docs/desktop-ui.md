# Desktop Clients & Local Bridge

Orangutan ships two complementary desktop experiences sharing the same warm parchment/terracotta editorial aesthetic and **Forest Companions** squad:

1. **Native Pure-Vulkan Desktop (`apps/desktop-vk/` → `orangutan-desktop`)** — zero-browser, C++26 + Vulkan 1.0 SDF renderer hosting `oran-bootstrap` directly in-process.
2. **Optional Browser Workbench (`apps/desktop/`)** — React 19 / TypeScript / Vite UI connecting over loopback HTTP/SSE (`/api`) to `orangutan serve`, with automatic offline demo fallback.

## Native Pure-Vulkan Desktop (`apps/desktop-vk/`)

`orangutan-desktop` renders directly on the GPU through a single instanced GLSL 450 SDF graphics pipeline (rounded cards, soft analytical shadows, hairline borders, crisp icons, FreeType2 CJK/Latin/Mono glyph atlas, and a 1536×1024 RGBA Vulkan texture atlas of photorealistic tactile Forest Companions with live studio dome specular & Fresnel rim lighting, plus procedural SDF fallback) with native Wayland/X11 windowing and CJK IME via SDL3.

| File | Responsibility |
| --- | --- |
| `shaders/ui.vert`, `shaders/ui.frag`, `shaders_spv.hpp` | Single-pass instanced SDF + R8 glyph-atlas shader compiled with `glslc` and embedded as SPIR-V. |
| `vk_abi.hpp`, `vk_renderer.hpp/.cpp` | Pure Vulkan 1.0 + `VK_KHR_swapchain` renderer with dynamic entry-point loading, 2 frames in flight, optional `VK_LAYER_KHRONOS_validation`, and headless offscreen PNG capture (`--screenshot`). |
| `font_atlas.hpp/.cpp` | FreeType2 + Fontconfig dynamic 2048×2048 R8 glyph atlas with CJK/Latin/Mono fallback and UTF-8 line wrapping. |
| `theme.hpp`, `primitives.hpp/.cpp` | Warm parchment light/dark tokens, SDF DrawList, crisp icons and 6 animated Forest Companions. |
| `agent_bridge.hpp/.cpp` | Direct in-process bridge to `oran-bootstrap` (`AgentSession`, `Fts5Backend`, `ReminderStore`, blocking `permission_ask_rendered` approvals, task-split teammate board). |
| `ui_app.hpp/.cpp`, `main.cpp` | Five clean views (Chat + Task-Split Board, Companions Squad, Memory, Reminders, Settings), CJK IME input and CLI options. |

## Companion Squad Model

Forest Companions (`Oran`, `Scout`, `Moss`, `Fern`, `Pebble`, `Pip`) represent **personable teammates and desk companions** (inspired by companion-first AI UX), **splitting complex goals into sub-tasks rather than splitting tools into narrow silos**. Users can choose any companion as their active desk buddy, interact with them, and watch sub-tasks progress in parallel while `Pebble` guards explicit write/shell confirmations.

## Browser Workbench (`apps/desktop/`)

| Path | Responsibility |
| --- | --- |
| `src/App.tsx`, `src/Assistant.tsx`, `src/TaskPanel.tsx` | Shell, conversation thread, task spine, interactive approvals and inspector. |
| `src/pages/{Tasks,Reminders,Connections,Memory,Settings}Page.tsx` | Modular views synced to `orangutan serve` when live or localStorage when offline. |
| `src/pets.tsx`, `src/runtime.ts`, `src/useAgent.ts` | Companion avatars, loopback HTTP/SSE client and dual-mode state owner. |
| `src/model.ts`, `src/schema.ts`, `src/components/Dialogs.tsx` | Pure state transitions, source isolation (`conversationKey`/`requesterKey`), Zod validation and dialogs. |

## Verification

```sh
# Native Pure-Vulkan desktop (requires system SDL3, FreeType2, Fontconfig, libpng16, libvulkan)
xmake build -j4 orangutan-desktop
build/linux/x86_64/release/orangutan-desktop --validate --showcase --screenshot /tmp/oran-vk.png

# Browser prototype checks
npm --prefix apps/desktop run build
npm --prefix apps/desktop test
npm --prefix apps/desktop run format:check
```
