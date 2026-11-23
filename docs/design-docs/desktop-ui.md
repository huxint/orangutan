# Pure-Vulkan Native Desktop (`apps/desktop-vk/`)

Orangutan ships a single, zero-browser **pure C++26 + Vulkan 1.0** native desktop application (`orangutan-desktop`) hosting `oran-bootstrap` directly in-process with zero web/Electron/HTML shell and zero external bitmap image dependencies.

## GPU Architecture

`orangutan-desktop` renders the entire UI and all six tactile 3D **Forest Companions** through a single instanced GLSL 450 graphics pipeline compiled to embedded SPIR-V (`shaders_spv.hpp`) with only two descriptor bindings (instance SSBO + dynamic FreeType2 R8 glyph atlas):

- **Analytical 2D UI SDFs**: rounded cards, soft analytical drop shadows, hairline borders, gradients, vector icons, and crisp FreeType2/Fontconfig CJK + Latin + Mono typography.
- **Pure-Code Real-Time 3D PBR Raymarched Figurines (`kind == 7`)**: every companion (`Oran`, `Scout`, `Moss`, `Fern`, `Pebble`, `Pip`) is sculpted and lit 100% procedurally in GLSL 450 via 3D SDF primitives, linear-space ACES filmic studio lighting, micro-fiber needle-felted wool normal perturbation, brushed antique brass, carved wood/ceramic, translucent backlit leaves, obsidian glass eyes with studio window reflections, real 3D animated blinking eyelids, and raymarched 3D soft shadows on an invisible floor plane (no backgrounds, no PNG cutouts).
- **Organic Autonomous 3D Motion & Gaze**: companions breathe calmly, blink periodically, track cursor position in 3D, and occasionally perform staggered autonomous 3D head turns, tilts, tail wags, leaf sways, and joyful hops on their own.

| File | Responsibility |
| --- | --- |
| `shaders/ui.vert`, `shaders/ui.frag`, `shaders_spv.hpp` | Single-pass instanced UI SDF + FreeType2 R8 glyph + real-time 3D PBR raymarched companion shader embedded as SPIR-V. |
| `vk_abi.hpp`, `vk_renderer.hpp/.cpp` | Pure Vulkan 1.0 + `VK_KHR_swapchain` renderer with dynamic `libvulkan` loading, 2 frames in flight, optional `VK_LAYER_KHRONOS_validation`, and headless offscreen PNG capture (`--screenshot`). |
| `font_atlas.hpp/.cpp` | FreeType2 + Fontconfig dynamic 2048×2048 R8 glyph atlas with CJK/Latin/Mono fallback and UTF-8 line wrapping. |
| `theme.hpp`, `primitives.hpp/.cpp` | Warm parchment light/dark tokens, SDF DrawList, crisp vector icons, and 3D skeletal pose/gaze controller (`compute_pose_3d`). |
| `agent_bridge.hpp/.cpp` | Direct in-process bridge to `oran-bootstrap` (`AgentSession`, `Fts5Backend`, `ReminderStore`, blocking `permission_ask_rendered` approvals, task-split teammate board). |
| `ui_app.hpp/.cpp`, `main.cpp` | Five native views (Chat + Task-Split Board, Companions Squad, Memory, Reminders, Settings), SDL3 Wayland/X11 windowing, CJK IME input, and CLI options. |

## Companion Squad Model

Forest Companions (`Oran`, `Scout`, `Moss`, `Fern`, `Pebble`, `Pip`) are **personable teammates and 3D desk companions**, **splitting complex goals into parallel sub-tasks rather than splitting tools into narrow silos**. Users can choose any companion as their active desk buddy, interact with them, and watch sub-tasks progress in parallel while `Pebble` guards explicit write/shell confirmations.

## Verification

```sh
# Requires system SDL3, FreeType2, Fontconfig, libpng16, and Vulkan driver (libvulkan)
xmake build -j4 orangutan-desktop
build/linux/x86_64/release/orangutan-desktop --validate --showcase --screenshot /tmp/oran-vk.png
```
