#include <chrono>
#include <cstdint>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "agent_bridge.hpp"
#include "font_atlas.hpp"
#include "primitives.hpp"
#include "ui_app.hpp"
#include "vk_renderer.hpp"

namespace {

using namespace orangutan::vkui;

struct LaunchOptions {
  std::string workspace_path{"."};
  std::string state_dir;
  std::string config_path;
  std::string screenshot_path;
  NavView initial_view{NavView::chat};
  std::uint32_t width{1280U};
  std::uint32_t height{820U};
  bool dark_mode{false};
  bool demo_mode{true};
  bool showcase{false};
  bool validate{false};
  bool help{false};
  float screenshot_time{0.0F};
};

void print_help() {
  std::println(
      "Orangutan Native Desktop (Pure C++26 + Vulkan)\n"
      "\n"
      "Usage:\n"
      "  orangutan-desktop [options]\n"
      "\n"
      "Options:\n"
      "  --workspace <dir>       Workspace root directory (default: current directory)\n"
      "  --state <dir>           Private SQLite state directory (default: <workspace>/.orangutan)\n"
      "  --config <file>         Provider config JSON (omit for zero-credential local demo)\n"
      "  --theme light|dark      Initial theme (default: light)\n"
      "  --view <name>           Initial view: chat|companions|memory|reminders|settings\n"
      "  --width <px>            Window/offscreen width (default: 1280)\n"
      "  --height <px>           Window/offscreen height (default: 820)\n"
      "  --screenshot <out.png>  Render one offscreen Vulkan frame directly to PNG and exit\n"
      "  --showcase              Populate realistic team task-split state (for preview/screenshot)\n"
      "  --validate              Enable VK_LAYER_KHRONOS_validation if available\n"
      "  --help, -h              Show this help");
}

NavView parse_view(std::string_view v) {
  if (v == "companions") {
    return NavView::companions;
  }
  if (v == "memory") {
    return NavView::memory;
  }
  if (v == "reminders") {
    return NavView::reminders;
  }
  if (v == "settings") {
    return NavView::settings;
  }
  return NavView::chat;
}

bool parse_args(int argc, char** argv, LaunchOptions& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      opts.help = true;
      return true;
    }
    if (a == "--demo") {
      opts.demo_mode = true;
    } else if (a == "--showcase") {
      opts.showcase = true;
    } else if (a == "--validate") {
      opts.validate = true;
    } else if (a == "--workspace" && i + 1 < argc) {
      opts.workspace_path = argv[++i];
    } else if (a == "--state" && i + 1 < argc) {
      opts.state_dir = argv[++i];
    } else if (a == "--config" && i + 1 < argc) {
      opts.config_path = argv[++i];
      opts.demo_mode = false;
    } else if (a == "--theme" && i + 1 < argc) {
      opts.dark_mode = std::string_view{argv[++i]} == "dark";
    } else if (a == "--view" && i + 1 < argc) {
      opts.initial_view = parse_view(argv[++i]);
    } else if (a == "--screenshot" && i + 1 < argc) {
      opts.screenshot_path = argv[++i];
    } else if (a == "--time" && i + 1 < argc) {
      opts.screenshot_time = std::stof(argv[++i]);
    } else if (a == "--width" && i + 1 < argc) {
      opts.width = static_cast<std::uint32_t>(std::max(640, std::stoi(argv[++i])));
    } else if (a == "--height" && i + 1 < argc) {
      opts.height = static_cast<std::uint32_t>(std::max(480, std::stoi(argv[++i])));
    } else {
      std::println(stderr, "Unknown option: {}", a);
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  LaunchOptions opts;
  if (!parse_args(argc, argv, opts)) {
    return 1;
  }
  if (opts.help) {
    print_help();
    return 0;
  }

  AgentBridge bridge;
  std::string err;
  if (!bridge.start(AgentBridgeOptions{
                        .workspace_path = opts.workspace_path,
                        .state_dir = opts.state_dir,
                        .config_path = opts.config_path,
                        .demo_mode = opts.demo_mode,
                    },
                    err)) {
    std::println(stderr, "Agent runtime error: {}", err);
    return 1;
  }
  if (opts.showcase) {
    bridge.seed_showcase_state();
  }

  // Headless pure Vulkan offscreen PNG capture path (zero display server required)
  if (!opts.screenshot_path.empty()) {
    VkRenderer renderer;
    if (!renderer.init_offscreen(opts.width, opts.height, opts.validate, err)) {
      std::println(stderr, "Vulkan offscreen init error: {}", err);
      bridge.stop();
      return 1;
    }

    FontAtlas atlas;
    if (!atlas.init(1.0F)) {
      std::println(stderr, "FreeType2 font initialization failed");
      renderer.shutdown();
      bridge.stop();
      return 1;
    }

    DesktopApp app{bridge, UiConfig{
                               .dark_mode = opts.dark_mode,
                               .animate_companions = opts.screenshot_time > 0.0F,
                               .initial_view = opts.initial_view,
                               .gpu_name = renderer.device_name(),
                           }};
    if (opts.screenshot_time > 0.0F) {
      app.tick(opts.screenshot_time);
    }
    DrawList dl;
    app.render(dl, atlas, static_cast<float>(opts.width), static_cast<float>(opts.height), 1.0F);
    if (!renderer.render_offscreen_png(dl, atlas, app.theme().bg, opts.screenshot_path, err)) {
      std::println(stderr, "Offscreen PNG render failed: {}", err);
      renderer.shutdown();
      bridge.stop();
      return 1;
    }

    std::println("Rendered Vulkan 3D PBR frame ({}x{}, device={}, validation={}) -> {}", opts.width, opts.height,
                 renderer.device_name(), renderer.validation_enabled() ? "on" : "off", opts.screenshot_path);
    renderer.shutdown();
    bridge.stop();
    return 0;
  }

  // Interactive native Wayland/X11 + Pure Vulkan window path
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
    bridge.stop();
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow("Orangutan — Personal Agent", static_cast<int>(opts.width),
                                        static_cast<int>(opts.height),
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window == nullptr) {
    std::println(stderr, "SDL_CreateWindow failed: {}", SDL_GetError());
    SDL_Quit();
    bridge.stop();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, 920, 620);
  SDL_StartTextInput(window);

  VkRenderer renderer;
  if (!renderer.init_window(window, opts.validate, err)) {
    std::println(stderr, "Vulkan init failed: {}", err);
    SDL_DestroyWindow(window);
    SDL_Quit();
    bridge.stop();
    return 1;
  }

  const float init_scale = SDL_GetWindowDisplayScale(window);
  FontAtlas atlas;
  if (!atlas.init(init_scale > 0.5F ? init_scale : 1.0F)) {
    std::println(stderr, "FreeType2 font init failed");
    renderer.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    bridge.stop();
    return 1;
  }

  DesktopApp app{bridge, UiConfig{
                             .dark_mode = opts.dark_mode,
                             .animate_companions = true,
                             .initial_view = opts.initial_view,
                             .gpu_name = renderer.device_name(),
                         }};

  DrawList dl;
  auto last_tick = std::chrono::steady_clock::now();
  bool running = true;

  while (running) {
    SDL_Event ev{};
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
          int pw = 0;
          int ph = 0;
          SDL_GetWindowSizeInPixels(window, &pw, &ph);
          if (pw > 0 && ph > 0) {
            renderer.on_resize(static_cast<std::uint32_t>(pw), static_cast<std::uint32_t>(ph));
          }
          const float dpr = SDL_GetWindowDisplayScale(window);
          if (dpr > 0.5F) {
            atlas.set_dpr(dpr);
          }
          break;
        }
        case SDL_EVENT_MOUSE_MOTION:
          app.on_mouse_move(ev.motion.x, ev.motion.y);
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (ev.button.button == SDL_BUTTON_LEFT) {
            app.on_mouse_down(ev.button.x, ev.button.y);
          }
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (ev.button.button == SDL_BUTTON_LEFT) {
            app.on_mouse_up(ev.button.x, ev.button.y);
          }
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          app.on_mouse_wheel(ev.wheel.y);
          break;
        case SDL_EVENT_TEXT_INPUT:
          if (ev.text.text != nullptr) {
            app.on_text_input(ev.text.text);
          }
          break;
        case SDL_EVENT_TEXT_EDITING:
          app.on_text_editing(ev.edit.text != nullptr ? ev.edit.text : "");
          break;
        case SDL_EVENT_KEY_DOWN: {
          const bool ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
          const bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
          if (ev.key.key == SDLK_BACKSPACE) {
            app.on_backspace();
          } else if (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) {
            app.on_enter(shift);
          } else if (ctrl && ev.key.key == SDLK_V) {
            if (char* clip = SDL_GetClipboardText()) {
              app.on_paste(clip);
              SDL_free(clip);
            }
          } else if (ev.key.key == SDLK_ESCAPE) {
            bridge.stop_active_turn();
          }
          break;
        }
        default:
          break;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.05F, std::chrono::duration<float>(now - last_tick).count());
    last_tick = now;
    app.tick(dt);

    int lw = 1280;
    int lh = 820;
    int pw = 1280;
    int ph = 820;
    SDL_GetWindowSize(window, &lw, &lh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    const float dpr = lw > 0 ? static_cast<float>(pw) / static_cast<float>(lw) : 1.0F;
    atlas.set_dpr(dpr);

    app.render(dl, atlas, static_cast<float>(lw), static_cast<float>(lh), dpr);
    renderer.render_frame(dl, atlas, app.theme().bg);
    SDL_Delay(12);
  }

  SDL_StopTextInput(window);
  renderer.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  bridge.stop();
  return 0;
}
