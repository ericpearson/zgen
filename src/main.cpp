// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
#include "config_dir.h"
#include "debug_flags.h"
#include "cheats/cheat_format.h"
#include "cheats/cheat_types.h"
#include "ui/app_ui.h"
#include "ui/game_renderer.h"
#include "ui/keybindings.h"

#include "imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#ifdef USE_VULKAN
#include "ui/vulkan_context.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
VulkanContext g_vkCtx; // global, referenced by game_renderer_vk.cpp
#else
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <algorithm>

bool g_debugMode = false;

// Default window size (3x native Genesis 320x224)
constexpr int DEFAULT_SCALE = 3;

// ---------- Config file persistence ----------

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static const char* fullscreenModeConfigValue(FullscreenMode mode) {
    switch (mode) {
        case FullscreenMode::Windowed:          return "windowed";
        case FullscreenMode::BorderlessDesktop: return "borderless";
        case FullscreenMode::Exclusive:         return "exclusive";
    }
    return "windowed";
}

static const char* videoStandardModeConfigValue(VideoStandardMode mode) {
    switch (mode) {
        case VideoStandardMode::Auto: return "auto";
        case VideoStandardMode::NTSC: return "ntsc";
        case VideoStandardMode::PAL:  return "pal";
    }
    return "auto";
}

static VideoStandard resolveVideoStandard(VideoStandardMode mode, const Cartridge& cartridge) {
    switch (mode) {
        case VideoStandardMode::NTSC:
            return VideoStandard::NTSC;
        case VideoStandardMode::PAL:
            return VideoStandard::PAL;
        case VideoStandardMode::Auto:
            return cartridge.isLoaded() ? cartridge.preferredVideoStandard() : VideoStandard::NTSC;
    }
    return VideoStandard::NTSC;
}

static FullscreenMode activeFullscreenMode(SDL_Window* window) {
    Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & SDL_WINDOW_FULLSCREEN) {
        return SDL_GetWindowFullscreenMode(window) ? FullscreenMode::Exclusive
                                                   : FullscreenMode::BorderlessDesktop;
    }
    return FullscreenMode::Windowed;
}

static std::string currentDisplayName(SDL_Window* window) {
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
    const char* name = displayId ? SDL_GetDisplayName(displayId) : nullptr;
    return name ? name : "Current Display";
}

static std::vector<AppUI::FullscreenResolution> queryFullscreenResolutions(SDL_Window* window) {
    std::vector<AppUI::FullscreenResolution> modes;

    SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
    if (!displayId) return modes;

    int numModes = 0;
    SDL_DisplayMode** displayModes = SDL_GetFullscreenDisplayModes(displayId, &numModes);
    if (!displayModes || numModes <= 0) {
        return modes;
    }

    for (int i = 0; i < numModes; i++) {
        const SDL_DisplayMode* mode = displayModes[i];
        if (!mode) {
            continue;
        }

        if (mode->w <= 0 || mode->h <= 0) {
            continue;
        }

        const int refreshHz = mode->refresh_rate > 0.0f
                                  ? static_cast<int>(mode->refresh_rate + 0.5f)
                                  : 0;
        const bool duplicate = std::any_of(modes.begin(), modes.end(),
                                           [&](const AppUI::FullscreenResolution& existing) {
                                               return existing.width == mode->w &&
                                                      existing.height == mode->h &&
                                                      existing.refreshHz == refreshHz;
                                           });
        if (duplicate) {
            continue;
        }

        AppUI::FullscreenResolution option;
        option.width = mode->w;
        option.height = mode->h;
        option.refreshHz = refreshHz;
        char label[64];
        if (refreshHz > 0) {
            std::snprintf(label, sizeof(label), "%d x %d @ %d Hz", mode->w, mode->h, refreshHz);
        } else {
            std::snprintf(label, sizeof(label), "%d x %d", mode->w, mode->h);
        }
        option.label = label;
        modes.push_back(option);
    }

    SDL_free(displayModes);

    std::sort(modes.begin(), modes.end(),
              [](const AppUI::FullscreenResolution& a, const AppUI::FullscreenResolution& b) {
                  const long long areaA = static_cast<long long>(a.width) * static_cast<long long>(a.height);
                  const long long areaB = static_cast<long long>(b.width) * static_cast<long long>(b.height);
                  if (areaA != areaB) return areaA > areaB;
                  if (a.width != b.width) return a.width > b.width;
                  if (a.height != b.height) return a.height > b.height;
                  return a.refreshHz > b.refreshHz;
              });

    return modes;
}

static int findFullscreenResolutionIndex(const std::vector<AppUI::FullscreenResolution>& modes,
                                         int width, int height, int refreshHz) {
    if (modes.empty()) {
        return -1;
    }

    int bestIndex = -1;
    int bestScore = 0x7fffffff;

    for (int i = 0; i < static_cast<int>(modes.size()); i++) {
        const auto& mode = modes[i];
        if (mode.width == width && mode.height == height &&
            (refreshHz <= 0 || mode.refreshHz == refreshHz)) {
            return i;
        }

        const int refreshPenalty = (refreshHz > 0 && mode.refreshHz > 0)
                                       ? std::abs(mode.refreshHz - refreshHz)
                                       : 0;
        const int score = std::abs(mode.width - width) +
                          std::abs(mode.height - height) +
                          (refreshPenalty * 4);
        if (score < bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }

    return bestIndex;
}

static bool queryExclusiveFullscreenMode(SDL_Window* window,
                                         int width,
                                         int height,
                                         int refreshHz,
                                         SDL_DisplayMode& outMode) {
    SDL_DisplayID displayId = SDL_GetDisplayForWindow(window);
    if (!displayId) {
        return false;
    }

    int numModes = 0;
    SDL_DisplayMode** displayModes = SDL_GetFullscreenDisplayModes(displayId, &numModes);
    if (!displayModes || numModes <= 0) {
        return false;
    }

    const SDL_DisplayMode* bestMode = nullptr;
    int bestScore = 0x7fffffff;
    for (int i = 0; i < numModes; i++) {
        const SDL_DisplayMode* mode = displayModes[i];
        if (!mode) {
            continue;
        }

        const int modeRefreshHz = mode->refresh_rate > 0.0f
                                      ? static_cast<int>(mode->refresh_rate + 0.5f)
                                      : 0;
        if (mode->w == width && mode->h == height &&
            (refreshHz <= 0 || modeRefreshHz == refreshHz)) {
            bestMode = mode;
            break;
        }

        const int refreshPenalty = (refreshHz > 0 && modeRefreshHz > 0)
                                       ? std::abs(modeRefreshHz - refreshHz)
                                       : 0;
        const int score = std::abs(mode->w - width) +
                          std::abs(mode->h - height) +
                          (refreshPenalty * 4);
        if (score < bestScore) {
            bestScore = score;
            bestMode = mode;
        }
    }

    if (bestMode) {
        outMode = *bestMode;
    }
    SDL_free(displayModes);
    return bestMode != nullptr;
}

static void applyWindowedSize(SDL_Window* window, int windowScale) {
    SDL_SetWindowFullscreen(window, false);
    SDL_SetWindowFullscreenMode(window, nullptr);
    SDL_SetWindowSize(window, 320 * windowScale, 224 * windowScale);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static bool applyPresentationMode(SDL_Window* window,
                                  FullscreenMode mode,
                                  int windowScale,
                                  const std::vector<AppUI::FullscreenResolution>& fullscreenModes,
                                  int& fullscreenResolutionIndex,
                                  int& fullscreenWidth,
                                  int& fullscreenHeight,
                                  int& fullscreenRefreshHz) {
    switch (mode) {
        case FullscreenMode::Windowed:
            applyWindowedSize(window, windowScale);
            return true;

        case FullscreenMode::BorderlessDesktop:
            SDL_SetWindowFullscreen(window, false);
            if (!SDL_SetWindowFullscreenMode(window, nullptr)) {
                return false;
            }
            return SDL_SetWindowFullscreen(window, true);

        case FullscreenMode::Exclusive: {
            if (fullscreenModes.empty()) {
                return false;
            }

            if (fullscreenResolutionIndex < 0 ||
                fullscreenResolutionIndex >= static_cast<int>(fullscreenModes.size())) {
                fullscreenResolutionIndex = findFullscreenResolutionIndex(
                    fullscreenModes, fullscreenWidth, fullscreenHeight, fullscreenRefreshHz);
                if (fullscreenResolutionIndex < 0) {
                    fullscreenResolutionIndex = 0;
                }
            }

            const auto& option = fullscreenModes[fullscreenResolutionIndex];
            fullscreenWidth = option.width;
            fullscreenHeight = option.height;
            fullscreenRefreshHz = option.refreshHz;

            SDL_DisplayMode modeRequest{};
            if (!queryExclusiveFullscreenMode(window,
                                              option.width,
                                              option.height,
                                              option.refreshHz,
                                              modeRequest)) {
                return false;
            }
            SDL_SetWindowFullscreen(window, false);
            if (!SDL_SetWindowFullscreenMode(window, &modeRequest)) {
                return false;
            }
            return SDL_SetWindowFullscreen(window, true);
        }
    }

    return false;
}

static void loadConfig(int& windowScale, ScalingMode& scalingMode, AspectRatio& aspectRatio,
                        bool& vsyncEnabled, bool& bilinearFiltering, bool& showFPS,
                        ProfilerMode& profilerMode,
                        VideoStandardMode& videoStandardMode,
                        bool& frameLimiterEnabled, int& audioQueueMs, FullscreenMode& fullscreenMode,
                        int& fullscreenWidth, int& fullscreenHeight, int& fullscreenRefreshHz,
                        bool& autoLoadState) {
    std::filesystem::path file = getConfigDir() / "config.ini";
    std::ifstream in(file);
    if (!in.is_open()) return;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                section = line.substr(1, end - 1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (section == "video") {
            if (key == "window_scale") {
                int v = std::atoi(val.c_str());
                if (v >= 1 && v <= 10) windowScale = v;
            } else if (key == "scaling_mode") {
                if (val == "integer") scalingMode = ScalingMode::IntegerOnly;
                else if (val == "fit") scalingMode = ScalingMode::Fit;
                else if (val == "stretch") scalingMode = ScalingMode::Stretch;
            } else if (key == "aspect_ratio") {
                if (val == "auto") aspectRatio = AspectRatio::Auto;
                else if (val == "4:3") aspectRatio = AspectRatio::FourThree;
                else if (val == "16:9") aspectRatio = AspectRatio::SixteenNine;
                else if (val == "stretch") aspectRatio = AspectRatio::Stretch;
            } else if (key == "fullscreen") {
                if (val == "true") fullscreenMode = FullscreenMode::BorderlessDesktop;
            } else if (key == "bilinear_filtering") {
                bilinearFiltering = (val == "true");
            } else if (key == "vsync") {
                vsyncEnabled = (val == "true");
            } else if (key == "show_fps") {
                showFPS = (val == "true");
            } else if (key == "profiler_mode") {
                profilerMode = (val == "detailed") ? ProfilerMode::Detailed : ProfilerMode::Simple;
            } else if (key == "fullscreen_mode") {
                if (val == "borderless") fullscreenMode = FullscreenMode::BorderlessDesktop;
                else if (val == "exclusive") fullscreenMode = FullscreenMode::Exclusive;
                else fullscreenMode = FullscreenMode::Windowed;
            } else if (key == "fullscreen_width") {
                fullscreenWidth = std::atoi(val.c_str());
            } else if (key == "fullscreen_height") {
                fullscreenHeight = std::atoi(val.c_str());
            } else if (key == "fullscreen_refresh") {
                fullscreenRefreshHz = std::atoi(val.c_str());
            }
        } else if (section == "audio") {
            if (key == "buffer_ms") {
                int v = std::atoi(val.c_str());
                if (v >= 10 && v <= 2000) audioQueueMs = v;
            }
        } else if (section == "emulation") {
            if (key == "frame_limiter") {
                frameLimiterEnabled = (val == "true");
            } else if (key == "video_standard") {
                if (val == "ntsc") videoStandardMode = VideoStandardMode::NTSC;
                else if (val == "pal") videoStandardMode = VideoStandardMode::PAL;
                else videoStandardMode = VideoStandardMode::Auto;
            } else if (key == "auto_load_state") {
                autoLoadState = (val == "true");
            }
        }
    }
}

static void saveConfig(int windowScale, ScalingMode scalingMode, AspectRatio aspectRatio,
                        bool vsyncEnabled, bool bilinearFiltering, bool showFPS,
                        ProfilerMode profilerMode,
                        VideoStandardMode videoStandardMode,
                        bool frameLimiterEnabled, int audioQueueMs, FullscreenMode fullscreenMode,
                        int fullscreenWidth, int fullscreenHeight, int fullscreenRefreshHz,
                        bool autoLoadState) {
    std::filesystem::path file = getConfigDir() / "config.ini";
    std::ofstream out(file);
    if (!out.is_open()) return;

    const char* scalingStr = "integer";
    switch (scalingMode) {
        case ScalingMode::IntegerOnly: scalingStr = "integer"; break;
        case ScalingMode::Fit:         scalingStr = "fit"; break;
        case ScalingMode::Stretch:     scalingStr = "stretch"; break;
    }

    const char* aspectStr = "auto";
    switch (aspectRatio) {
        case AspectRatio::Auto:        aspectStr = "auto"; break;
        case AspectRatio::FourThree:   aspectStr = "4:3"; break;
        case AspectRatio::SixteenNine: aspectStr = "16:9"; break;
        case AspectRatio::Stretch:     aspectStr = "stretch"; break;
    }

    out << "[video]\n";
    out << "window_scale = " << windowScale << "\n";
    out << "scaling_mode = " << scalingStr << "\n";
    out << "aspect_ratio = " << aspectStr << "\n";
    out << "fullscreen_mode = " << fullscreenModeConfigValue(fullscreenMode) << "\n";
    out << "fullscreen_width = " << fullscreenWidth << "\n";
    out << "fullscreen_height = " << fullscreenHeight << "\n";
    out << "fullscreen_refresh = " << fullscreenRefreshHz << "\n";
    out << "bilinear_filtering = " << (bilinearFiltering ? "true" : "false") << "\n";
    out << "vsync = " << (vsyncEnabled ? "true" : "false") << "\n";
    out << "show_fps = " << (showFPS ? "true" : "false") << "\n";
    out << "profiler_mode = " << (profilerMode == ProfilerMode::Detailed ? "detailed" : "simple") << "\n";
    out << "\n[audio]\n";
    out << "buffer_ms = " << audioQueueMs << "\n";
    out << "\n[emulation]\n";
    out << "frame_limiter = " << (frameLimiterEnabled ? "true" : "false") << "\n";
    out << "video_standard = " << videoStandardModeConfigValue(videoStandardMode) << "\n";
    out << "auto_load_state = " << (autoLoadState ? "true" : "false") << "\n";
}

static float ticksToMs(u64 ticks, u64 perfFreq) {
    if (perfFreq == 0) return 0.0f;
    return static_cast<float>((static_cast<double>(ticks) * 1000.0) /
                              static_cast<double>(perfFreq));
}

// Key mappings using rebindable keybindings
static void handleKeyEvent(Genesis& genesis, SDL_Keycode key, bool pressed, const KeyBindings& kb) {
    if (key == kb.get(BindableAction::PadUp))       genesis.setButton(0, 0, pressed);
    if (key == kb.get(BindableAction::PadDown))     genesis.setButton(0, 1, pressed);
    if (key == kb.get(BindableAction::PadLeft))     genesis.setButton(0, 2, pressed);
    if (key == kb.get(BindableAction::PadRight))    genesis.setButton(0, 3, pressed);
    if (key == kb.get(BindableAction::ButtonA))     genesis.setButton(0, 4, pressed);
    if (key == kb.get(BindableAction::ButtonB))     genesis.setButton(0, 5, pressed);
    if (key == kb.get(BindableAction::ButtonC))     genesis.setButton(0, 6, pressed);
    if (key == kb.get(BindableAction::ButtonStart)) genesis.setButton(0, 7, pressed);
    if (key == kb.get(BindableAction::ButtonX))     genesis.setButton(0, 8, pressed);
    if (key == kb.get(BindableAction::ButtonY))     genesis.setButton(0, 9, pressed);
    if (key == kb.get(BindableAction::ButtonZ))     genesis.setButton(0, 10, pressed);
    if (key == kb.get(BindableAction::ButtonMode))  genesis.setButton(0, 11, pressed);
}

static void releaseAllButtons(Genesis& genesis) {
    for (int button = 0; button < 12; button++) {
        genesis.setButton(0, button, false);
        genesis.setButton(1, button, false);
    }
}

// ---------- Gamepad support ----------

static SDL_Gamepad* gamepads[2] = { nullptr, nullptr };
static SDL_JoystickID gamepadIDs[2] = { 0, 0 };
// Track stick-derived d-pad state per port so we can release properly
static bool stickUp[2] = {}, stickDown[2] = {}, stickLeft[2] = {}, stickRight[2] = {};

static int gamepadPort(SDL_JoystickID id) {
    for (int i = 0; i < 2; i++)
        if (gamepads[i] && gamepadIDs[i] == id) return i;
    return -1;
}

static void openGamepad(SDL_JoystickID instanceID) {
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (!gamepads[i]) { slot = i; break; }
    }
    if (slot < 0) return; // both ports occupied

    SDL_Gamepad* gp = SDL_OpenGamepad(instanceID);
    if (!gp) return;

    gamepads[slot] = gp;
    gamepadIDs[slot] = instanceID;
    const char* name = SDL_GetGamepadName(gp);
    printf("Gamepad connected to port %d: %s\n", slot + 1, name ? name : "Unknown");
}

static void closeGamepad(SDL_JoystickID instanceID) {
    for (int i = 0; i < 2; i++) {
        if (gamepads[i] && gamepadIDs[i] == instanceID) {
            printf("Gamepad disconnected from port %d\n", i + 1);
            SDL_CloseGamepad(gamepads[i]);
            gamepads[i] = nullptr;
            gamepadIDs[i] = 0;
            stickUp[i] = stickDown[i] = stickLeft[i] = stickRight[i] = false;
            return;
        }
    }
}

static void handleGamepadButton(Genesis& genesis, int port, SDL_GamepadButton btn, bool pressed) {
    // Xbox-style → Genesis mapping:
    //   A(bottom)→B, B(right)→C, X(left)→A, Y(top)→Y
    //   LB→X, RB→Z, Start→Start (Back/View toggles UI menu)
    switch (btn) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:       genesis.setButton(port, 0, pressed); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:     genesis.setButton(port, 1, pressed); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:     genesis.setButton(port, 2, pressed); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:    genesis.setButton(port, 3, pressed); break;
        case SDL_GAMEPAD_BUTTON_SOUTH:         genesis.setButton(port, 5, pressed); break; // A → B
        case SDL_GAMEPAD_BUTTON_EAST:          genesis.setButton(port, 6, pressed); break; // B → C
        case SDL_GAMEPAD_BUTTON_WEST:          genesis.setButton(port, 4, pressed); break; // X → A
        case SDL_GAMEPAD_BUTTON_NORTH:         genesis.setButton(port, 9, pressed); break; // Y → Y
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: genesis.setButton(port, 8, pressed); break; // LB → X
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:genesis.setButton(port, 10, pressed); break; // RB → Z
        case SDL_GAMEPAD_BUTTON_START:         genesis.setButton(port, 7, pressed); break;
        default: break;
    }
}

static constexpr int STICK_DEADZONE = 8000;

static void handleGamepadAxis(Genesis& genesis, int port, SDL_GamepadAxis axis, int value) {
    if (axis == SDL_GAMEPAD_AXIS_LEFTX) {
        bool left = value < -STICK_DEADZONE;
        bool right = value > STICK_DEADZONE;
        if (left != stickLeft[port]) { genesis.setButton(port, 2, left); stickLeft[port] = left; }
        if (right != stickRight[port]) { genesis.setButton(port, 3, right); stickRight[port] = right; }
    } else if (axis == SDL_GAMEPAD_AXIS_LEFTY) {
        bool up = value < -STICK_DEADZONE;
        bool down = value > STICK_DEADZONE;
        if (up != stickUp[port]) { genesis.setButton(port, 0, up); stickUp[port] = up; }
        if (down != stickDown[port]) { genesis.setButton(port, 1, down); stickDown[port] = down; }
    }
}

int main(int argc, char* argv[]) {
    const char* startupRom = nullptr;
    const char* startupState = nullptr;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printf("Usage: genesis [options] [rom]\n\n");
            printf("Options:\n");
            printf("  --state <file>       Load a save state file on startup\n");
            printf("  --config-dir <path>  Use a custom config/save directory (default: ~/.genesis)\n");
            printf("  --debug              Enable debug logging (GENESIS_LOG_* env vars)\n");
            printf("  -h, --help           Show this help message\n\n");
            printf("Supported ROM formats: .bin, .md, .gen, .smd\n");
            printf("Set GENESIS_MAX_FRAMES=N to stop after N frames.\n");
            return 0;
        } else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            startupState = argv[++i];
        } else if (std::strcmp(argv[i], "--config-dir") == 0 && i + 1 < argc) {
            setConfigDir(argv[++i]);
        } else if (std::strcmp(argv[i], "--debug") == 0) {
            g_debugMode = true;
        } else if (!startupRom) {
            startupRom = argv[i];
        }
    }

    // ---------- SDL init ----------

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        printf("SDL could not initialize: %s\n", SDL_GetError());
        return 1;
    }

    int windowWidth = 320 * DEFAULT_SCALE;
    int windowHeight = 224 * DEFAULT_SCALE;

#ifdef USE_VULKAN
    // ---------- SDL + Vulkan window ----------
    SDL_Window* window = SDL_CreateWindow(
        "Genesis Emulator",
        windowWidth, windowHeight,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        printf("Window could not be created: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    if (!g_vkCtx.init(window)) {
        printf("Vulkan initialization failed\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    printf("Vulkan renderer initialized\n");
#else
    // ---------- SDL + OpenGL window ----------
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window = SDL_CreateWindow(
        "Genesis Emulator",
        windowWidth, windowHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
    );
    if (!window) {
        printf("Window could not be created: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        printf("OpenGL context could not be created: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, glContext);

#ifndef __APPLE__
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        fprintf(stderr, "GLEW init failed: %s\n", glewGetErrorString(glewErr));
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
#endif

    SDL_GL_SetSwapInterval(0); // VSync off — we do our own frame pacing

    printf("OpenGL %s\n", glGetString(GL_VERSION));
#endif

    // ---------- Audio ----------

    SDL_AudioSpec audioSpec;
    SDL_zero(audioSpec);
    audioSpec.freq = 48000;
    audioSpec.format = SDL_AUDIO_S16;
    audioSpec.channels = 2;

    SDL_AudioStream* audioStream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audioSpec, nullptr, nullptr
    );
    int audioRate = audioSpec.freq;
    u32 audioBytesPerSecond = static_cast<u32>(audioRate) * audioSpec.channels * sizeof(s16);
    if (!audioStream) {
        printf("Warning: Could not open audio device: %s\n", SDL_GetError());
    } else {
        SDL_AudioSpec streamInputSpec{};
        SDL_AudioSpec deviceSpec{};
        int sampleFrames = 0;
        if (!SDL_GetAudioStreamFormat(audioStream, &streamInputSpec, &deviceSpec)) {
            streamInputSpec = audioSpec;
            SDL_zero(deviceSpec);
        }
        SDL_AudioDeviceID audioDevice = SDL_GetAudioStreamDevice(audioStream);
        if (audioDevice != 0) {
            SDL_GetAudioDeviceFormat(audioDevice, &deviceSpec, &sampleFrames);
        }
        audioRate = streamInputSpec.freq;
        audioBytesPerSecond = static_cast<u32>(audioRate) * streamInputSpec.channels * sizeof(s16);
        printf("Audio stream: %d Hz input, %d channels, device %d Hz, %d sample buffer\n",
               streamInputSpec.freq, streamInputSpec.channels, deviceSpec.freq, sampleFrames);
        SDL_ResumeAudioStreamDevice(audioStream);
    }

    // ---------- Emulator ----------

    Genesis genesis;
    genesis.setAudioSampleRate(audioRate);
    genesis.reset();

    // ---------- UI + Renderer init ----------

    GameRenderer gameRenderer;
    gameRenderer.init(genesis.getScreenWidth(), genesis.getScreenHeight());

    AppUI ui;
#ifdef USE_VULKAN
    ui.init(window, nullptr);

    // Complete Vulkan ImGui init
    ImGui_ImplVulkan_InitInfo vkInitInfo{};
    vkInitInfo.ApiVersion = VK_API_VERSION_1_0;
    vkInitInfo.Instance = g_vkCtx.instance();
    vkInitInfo.PhysicalDevice = g_vkCtx.physicalDevice();
    vkInitInfo.Device = g_vkCtx.device();
    vkInitInfo.QueueFamily = g_vkCtx.queueFamily();
    vkInitInfo.Queue = g_vkCtx.queue();
    vkInitInfo.DescriptorPool = g_vkCtx.descriptorPool();
    vkInitInfo.MinImageCount = g_vkCtx.minImageCount();
    vkInitInfo.ImageCount = g_vkCtx.imageCount();
    vkInitInfo.PipelineInfoMain.RenderPass = g_vkCtx.renderPass();
    ImGui_ImplVulkan_Init(&vkInitInfo);
#else
    ui.init(window, glContext);
#endif

    KeyBindings keyBindings;
    keyBindings.load();
    ui.setKeyBindings(&keyBindings);

    int windowScale = DEFAULT_SCALE;
    ScalingMode scalingMode = ScalingMode::IntegerOnly;
    AspectRatio aspectRatio = AspectRatio::Auto;
    bool vsyncEnabled = false;
    bool bilinearFiltering = false;
    VideoStandardMode videoStandardMode = VideoStandardMode::Auto;
    FullscreenMode fullscreenMode = FullscreenMode::Windowed;
    int fullscreenWidth = 0;
    int fullscreenHeight = 0;
    int fullscreenRefreshHz = 0;
    std::vector<AppUI::FullscreenResolution> fullscreenResolutions;
    int fullscreenResolutionIndex = -1;

    // ---------- Main loop ----------

    bool running = true;
    bool frameLimiterEnabled = true;
    bool pauseWasActiveBeforeMenu = false;
    bool menuPauseEngaged = false;
    bool showFPS = false;
    ProfilerMode profilerMode = ProfilerMode::Simple;
    bool fastForwardHeld = false;
    int fastForwardMultiplier = 5;  // Run N frames per render when fast-forwarding
    int audioQueueMs = 250;
    bool romLoaded = false;
    bool autoLoadState = false;

    // Load persisted settings from config.ini
    loadConfig(windowScale, scalingMode, aspectRatio, vsyncEnabled,
               bilinearFiltering, showFPS, profilerMode, videoStandardMode, frameLimiterEnabled, audioQueueMs,
               fullscreenMode, fullscreenWidth, fullscreenHeight, fullscreenRefreshHz, autoLoadState);
    genesis.setVideoStandard(resolveVideoStandard(videoStandardMode, genesis.getCartridge()));
    genesis.setDetailedProfiling(showFPS && profilerMode == ProfilerMode::Detailed);
    ui.setVideoStandardMode(videoStandardMode);
    ui.setAutoLoadState(autoLoadState);
    ui.setEffectiveVideoTiming(genesis.getVideoStandard(), genesis.getFrameRate(), genesis.getScanlinesPerFrame());

    auto refreshFullscreenState = [&]() {
        fullscreenResolutions = queryFullscreenResolutions(window);
        fullscreenResolutionIndex = findFullscreenResolutionIndex(
            fullscreenResolutions, fullscreenWidth, fullscreenHeight, fullscreenRefreshHz);
        if (fullscreenResolutionIndex < 0 && !fullscreenResolutions.empty()) {
            fullscreenResolutionIndex = 0;
            fullscreenWidth = fullscreenResolutions[0].width;
            fullscreenHeight = fullscreenResolutions[0].height;
            fullscreenRefreshHz = fullscreenResolutions[0].refreshHz;
        }

        ui.setFullscreenMode(fullscreenMode);
        ui.setFullscreenResolutions(fullscreenResolutions, fullscreenResolutionIndex, currentDisplayName(window));
    };

    // Apply loaded settings
    applyWindowedSize(window, windowScale);
    ui.setWindowScale(windowScale);
    ui.setProfilerMode(profilerMode);
    gameRenderer.setFilterMode(bilinearFiltering);
#ifndef USE_VULKAN
    SDL_GL_SetSwapInterval(vsyncEnabled ? 1 : 0);
#endif
    refreshFullscreenState();
    if (fullscreenMode != FullscreenMode::Windowed) {
        if (!applyPresentationMode(window, fullscreenMode, windowScale, fullscreenResolutions,
                                   fullscreenResolutionIndex, fullscreenWidth,
                                   fullscreenHeight, fullscreenRefreshHz)) {
            fullscreenMode = FullscreenMode::BorderlessDesktop;
            if (!applyPresentationMode(window, fullscreenMode, windowScale, fullscreenResolutions,
                                       fullscreenResolutionIndex, fullscreenWidth,
                                       fullscreenHeight, fullscreenRefreshHz)) {
                fullscreenMode = FullscreenMode::Windowed;
                applyWindowedSize(window, windowScale);
            }
        }
        refreshFullscreenState();
    }

    int currentSaveSlot = 0;
    int fpsCount = 0;
    int displayFPS = 0;
    u64 fpsTimer = SDL_GetPerformanceCounter();
    const u64 perfFreq = SDL_GetPerformanceFrequency();
    double targetFps = genesis.getFrameRate();
    u64 frameTicks = static_cast<u64>(perfFreq / targetFps);
    u64 nextFrameTick = SDL_GetPerformanceCounter() + frameTicks;
    AppUI::PerformanceStats profilerStats{};
    bool profilerStatsInitialized = false;

    auto refreshTimingState = [&]() {
        targetFps = genesis.getFrameRate();
        frameTicks = static_cast<u64>(perfFreq / targetFps);
        nextFrameTick = SDL_GetPerformanceCounter() + frameTicks;
        ui.setVideoStandardMode(videoStandardMode);
        ui.setEffectiveVideoTiming(genesis.getVideoStandard(),
                                   genesis.getFrameRate(),
                                   genesis.getScanlinesPerFrame());
    };

    auto updateWindowTitle = [&]() {
        if (romLoaded) {
            std::string title = "Genesis - " + genesis.getCartridge().getGameName();
            SDL_SetWindowTitle(window, title.c_str());
        } else {
            SDL_SetWindowTitle(window, "Genesis - No ROM loaded");
        }
    };

    auto loadRomIntoEmulator = [&](const char* path) -> bool {
        if (!path || !*path) {
            return false;
        }
        if (!genesis.loadROM(path)) {
            return false;
        }
        romLoaded = true;
        genesis.setVideoStandard(resolveVideoStandard(videoStandardMode, genesis.getCartridge()));
        genesis.setRomPath(path);
        genesis.clearCheats();
        genesis.resetRamSearch();
        std::string cheatError;
        size_t loadedCheatCount = 0;
        if (!genesis.loadCheatsFromFile(&cheatError, &loadedCheatCount)) {
            std::printf("Failed to load cheat sidecar: %s\n", cheatError.c_str());
        }
        ui.setCurrentRomPath(path);
        ui.addRecentRom(path);
        refreshTimingState();
        updateWindowTitle();
        return true;
    };

    auto smoothProfilerStats = [&](const AppUI::PerformanceStats& sample) {
        profilerStats.fps = sample.fps;
        if (!profilerStatsInitialized) {
            profilerStats = sample;
            profilerStatsInitialized = true;
            return;
        }

        constexpr float alpha = 0.15f;
        auto smooth = [&](float& dst, float src) {
            dst += (src - dst) * alpha;
        };

        smooth(profilerStats.frameMs, sample.frameMs);
        smooth(profilerStats.busyMs, sample.busyMs);
        smooth(profilerStats.sdlMs, sample.sdlMs);
        smooth(profilerStats.emulationMs, sample.emulationMs);
        smooth(profilerStats.cheatsMs, sample.cheatsMs);
        smooth(profilerStats.m68kMs, sample.m68kMs);
        smooth(profilerStats.z80Ms, sample.z80Ms);
        smooth(profilerStats.vdpMs, sample.vdpMs);
        smooth(profilerStats.ymMs, sample.ymMs);
        smooth(profilerStats.psgMs, sample.psgMs);
        smooth(profilerStats.mixMs, sample.mixMs);
        smooth(profilerStats.emulationOtherMs, sample.emulationOtherMs);
        smooth(profilerStats.audioMs, sample.audioMs);
        smooth(profilerStats.videoMs, sample.videoMs);
        smooth(profilerStats.uiMs, sample.uiMs);
        smooth(profilerStats.presentMs, sample.presentMs);
        smooth(profilerStats.pacingMs, sample.pacingMs);
        smooth(profilerStats.otherMs, sample.otherMs);
    };

    auto syncCheatsToUI = [&]() {
        std::vector<AppUI::CheatItem> items;
        const auto& cheats = genesis.getCheats();
        items.reserve(cheats.size());
        for (const auto& cheat : cheats) {
            AppUI::CheatItem item;
            item.code = cheat.code;
            item.name = cheat.name;
            item.enabled = cheat.enabled;
            items.push_back(item);
        }
        ui.setCheats(items);
    };

    auto syncRamSearchToUI = [&]() {
        std::vector<AppUI::RamSearchResultItem> items;
        std::vector<Genesis::SearchCandidate> results;
        results.reserve(500);
        genesis.getRamSearchResults(0, 500, results);
        const auto valueType = genesis.getRamSearchValueType();
        items.reserve(results.size());
        for (size_t i = 0; i < results.size(); i++) {
            const auto& result = results[i];
            AppUI::RamSearchResultItem item;
            item.index = static_cast<int>(i);
            item.address = result.where.address;
            item.region = result.where.region;
            item.regionLabel = Cheats::regionLabel(result.where.region);
            item.currentDec = Cheats::formatValueDecimal(result.currentValue, valueType);
            item.currentHex = Cheats::formatValueHex(result.currentValue, valueType);
            item.previousDec = Cheats::formatValueDecimal(result.previousValue, valueType);
            item.previousHex = Cheats::formatValueHex(result.previousValue, valueType);
            item.typeLabel = Cheats::valueTypeLabel(valueType);
            items.push_back(item);
        }
        ui.setRamSearchActive(genesis.isRamSearchActive());
        ui.setRamSearchResultCount(static_cast<int>(genesis.getRamSearchResultCount()));
        ui.setRamSearchResults(items);
    };

    auto parseWriteValue = [&](const std::string& text,
                               Genesis::CheatValueType type,
                               u32& outRawValue) -> bool {
        s32 parsed = 0;
        if (!Cheats::parseSearchValueText(text, parsed)) {
            return false;
        }
        switch (type) {
            case Genesis::CheatValueType::U8:
                if (parsed < 0 || parsed > 0xFF) return false;
                outRawValue = static_cast<u32>(parsed);
                return true;
            case Genesis::CheatValueType::S8:
                if (parsed < -128 || parsed > 127) return false;
                outRawValue = static_cast<u8>(static_cast<s8>(parsed));
                return true;
            case Genesis::CheatValueType::U16:
                if (parsed < 0 || parsed > 0xFFFF) return false;
                outRawValue = static_cast<u32>(parsed);
                return true;
            case Genesis::CheatValueType::S16:
                if (parsed < -32768 || parsed > 32767) return false;
                outRawValue = static_cast<u16>(static_cast<s16>(parsed));
                return true;
            case Genesis::CheatValueType::U32:
                if (parsed < 0) return false;
                outRawValue = static_cast<u32>(parsed);
                return true;
            case Genesis::CheatValueType::S32:
                outRawValue = static_cast<u32>(parsed);
                return true;
        }
        return false;
    };

    syncCheatsToUI();
    syncRamSearchToUI();
    if (startupRom && loadRomIntoEmulator(startupRom)) {
        syncCheatsToUI();
        syncRamSearchToUI();
        ui.setRomLoaded(true);
        if (startupState) {
            if (genesis.loadStateFromFile(startupState)) {
                printf("Loaded save state: %s\n", startupState);
            } else {
                printf("Failed to load save state: %s\n", startupState);
            }
        } else if (autoLoadState) {
            int slot = genesis.findLatestStateSlot();
            if (slot >= 0 && genesis.loadState(slot)) {
                ui.setStatus("Auto-loaded slot " + std::to_string(slot));
                if (audioStream) SDL_ClearAudioStream(audioStream);
            }
        }
        std::string gameName = genesis.getCartridge().getGameName();
        std::string region = genesis.getCartridge().getRegion();
        char checksumStr[8];
        std::snprintf(checksumStr, sizeof(checksumStr), "%04X",
                      genesis.getCartridge().getHeader().checksum);
        std::string info = gameName;
        if (!region.empty()) info += " [" + region + "]";
        info += " (" + std::string(checksumStr) + ")";
        ui.setStatus(info);
    } else {
        ui.setRomLoaded(false);
        if (startupRom) {
            ui.setStatus("Failed to load ROM");
            printf("Failed to load ROM: %s\n", startupRom);
        } else {
            ui.setStatus("No ROM loaded");
        }
        ui.setVisible(true);
        ui.openFileBrowser();
        refreshTimingState();
        updateWindowTitle();
    }

    auto applyAction = [&](const AppUI::Action& action) {
        switch (action.type) {
            case AppUI::ActionType::None:
                break;

            case AppUI::ActionType::Quit:
                running = false;
                break;

            case AppUI::ActionType::Reset:
                genesis.reset();
                genesis.resetRamSearch();
                genesis.setVideoStandard(resolveVideoStandard(videoStandardMode, genesis.getCartridge()));
                if (menuPauseEngaged) {
                    genesis.setPaused(false);
                    menuPauseEngaged = false;
                }
                if (audioStream) SDL_ClearAudioStream(audioStream);
                refreshTimingState();
                syncRamSearchToUI();
                ui.setStatus("Console reset");
                break;

            case AppUI::ActionType::LoadRom:
                if (loadRomIntoEmulator(action.text.c_str())) {
                    syncCheatsToUI();
                    syncRamSearchToUI();
                    ui.setRomLoaded(true);
                    {
                        std::string gameName = genesis.getCartridge().getGameName();
                        std::string region = genesis.getCartridge().getRegion();
                        char checksumStr[8];
                        std::snprintf(checksumStr, sizeof(checksumStr), "%04X",
                                      genesis.getCartridge().getHeader().checksum);
                        std::string info = gameName;
                        if (!region.empty()) info += " [" + region + "]";
                        info += " (" + std::string(checksumStr) + ")";
                        ui.setStatus(info);
                    }
                    if (audioStream) SDL_ClearAudioStream(audioStream);
                    if (autoLoadState) {
                        int slot = genesis.findLatestStateSlot();
                        if (slot >= 0 && genesis.loadState(slot)) {
                            ui.setStatus("Auto-loaded slot " + std::to_string(slot));
                            if (audioStream) SDL_ClearAudioStream(audioStream);
                        }
                    }
                    if (menuPauseEngaged) {
                        genesis.setPaused(pauseWasActiveBeforeMenu);
                        menuPauseEngaged = false;
                    }
                } else {
                    ui.setStatus("Failed to load ROM");
                }
                break;

            case AppUI::ActionType::SetFrameLimiter:
                frameLimiterEnabled = action.boolValue;
                if (frameLimiterEnabled) {
                    nextFrameTick = SDL_GetPerformanceCounter() + frameTicks;
                }
                break;

            case AppUI::ActionType::SetWindowScale:
                windowScale = action.intValue;
                if (fullscreenMode == FullscreenMode::Windowed) {
                    applyWindowedSize(window, windowScale);
                }
                break;

            case AppUI::ActionType::SetAudioQueueMs:
                audioQueueMs = action.intValue;
                break;

            case AppUI::ActionType::ToggleFPS:
                showFPS = action.boolValue;
                genesis.setDetailedProfiling(showFPS && profilerMode == ProfilerMode::Detailed);
                break;

            case AppUI::ActionType::SetProfilerMode:
                profilerMode = (action.intValue == static_cast<int>(ProfilerMode::Detailed))
                    ? ProfilerMode::Detailed
                    : ProfilerMode::Simple;
                genesis.setDetailedProfiling(showFPS && profilerMode == ProfilerMode::Detailed);
                break;

            case AppUI::ActionType::AddCheat: {
                std::string errorText;
                if (genesis.addCheat(action.text, action.text2, &errorText)) {
                    ui.setStatus("Cheat added");
                } else {
                    ui.setStatus(errorText.empty() ? "Invalid cheat" : errorText);
                }
                syncCheatsToUI();
                break;
            }

            case AppUI::ActionType::ToggleCheat:
                if (action.intValue >= 0 &&
                    genesis.toggleCheat(static_cast<size_t>(action.intValue))) {
                    ui.setStatus("Cheat toggled");
                } else {
                    ui.setStatus("Cheat not found");
                }
                syncCheatsToUI();
                break;

            case AppUI::ActionType::RemoveCheat:
                if (action.intValue >= 0 &&
                    genesis.removeCheat(static_cast<size_t>(action.intValue))) {
                    ui.setStatus("Cheat removed");
                } else {
                    ui.setStatus("Cheat not found");
                }
                syncCheatsToUI();
                break;

            case AppUI::ActionType::ClearCheats:
                genesis.clearCheats();
                syncCheatsToUI();
                ui.setStatus("Cheats cleared");
                break;

            case AppUI::ActionType::LoadCheatsFromFile: {
                if (!romLoaded) {
                    ui.setStatus("Load a ROM before loading cheats");
                    break;
                }
                std::string errorText;
                size_t loadedCount = 0;
                if (genesis.loadCheatsFromFile(&errorText, &loadedCount)) {
                    syncCheatsToUI();
                    ui.setStatus("Loaded " + std::to_string(loadedCount) + " cheat(s)");
                } else {
                    ui.setStatus(errorText.empty() ? "Failed to load cheats" : errorText);
                }
                break;
            }

            case AppUI::ActionType::SaveCheatsToFile: {
                if (!romLoaded) {
                    ui.setStatus("Load a ROM before saving cheats");
                    break;
                }
                std::string errorText;
                if (genesis.saveCheatsToFile(&errorText)) {
                    ui.setStatus("Saved cheats to ROM sidecar");
                } else {
                    ui.setStatus(errorText.empty() ? "Failed to save cheats" : errorText);
                }
                break;
            }

            case AppUI::ActionType::StartRamSearchKnownValue: {
                if (!romLoaded || !ui.isVisible()) {
                    ui.setStatus("Open the menu with a ROM loaded to use RAM Search");
                    break;
                }
                s32 value = 0;
                if (!Cheats::parseSearchValueText(action.text, value)) {
                    ui.setStatus("Enter a valid search value");
                    break;
                }
                if (genesis.startRamSearchKnownValue(
                        static_cast<Genesis::CheatValueType>(action.intValue),
                        value,
                        static_cast<Genesis::SearchHeuristic>(action.intValue2))) {
                    ui.setStatus("RAM search started");
                } else {
                    ui.setStatus("No writable memory available to search");
                }
                syncRamSearchToUI();
                break;
            }

            case AppUI::ActionType::StartRamSearchUnknown:
                if (!romLoaded || !ui.isVisible()) {
                    ui.setStatus("Open the menu with a ROM loaded to use RAM Search");
                    break;
                }
                if (genesis.startRamSearchUnknown(static_cast<Genesis::CheatValueType>(action.intValue))) {
                    ui.setStatus("Initial RAM snapshot captured");
                } else {
                    ui.setStatus("No writable memory available to search");
                }
                syncRamSearchToUI();
                break;

            case AppUI::ActionType::RefineRamSearch: {
                if (!romLoaded || !ui.isVisible()) {
                    ui.setStatus("Open the menu with a ROM loaded to use RAM Search");
                    break;
                }
                const auto compareMode = static_cast<Genesis::SearchCompareMode>(action.intValue);
                std::optional<s32> compareValue;
                if (compareMode == Genesis::SearchCompareMode::EqualToValue ||
                    compareMode == Genesis::SearchCompareMode::NotEqualToValue) {
                    s32 parsed = 0;
                    if (!Cheats::parseSearchValueText(action.text, parsed)) {
                        ui.setStatus("Enter a valid comparison value");
                        break;
                    }
                    compareValue = parsed;
                }
                if (genesis.refineRamSearch(compareMode,
                                            compareValue,
                                            static_cast<Genesis::SearchHeuristic>(action.intValue2))) {
                    ui.setStatus("RAM search refined");
                } else {
                    ui.setStatus("RAM search is not active");
                }
                syncRamSearchToUI();
                break;
            }

            case AppUI::ActionType::ResetRamSearch:
                genesis.resetRamSearch();
                syncRamSearchToUI();
                ui.setStatus("RAM search reset");
                break;

            case AppUI::ActionType::SetRamSearchResultOnce: {
                u32 rawValue = 0;
                if (!parseWriteValue(action.text, genesis.getRamSearchValueType(), rawValue)) {
                    ui.setStatus("Enter a valid value for the selected type");
                    break;
                }
                if (action.intValue >= 0 &&
                    genesis.setRamSearchResultValueOnce(static_cast<size_t>(action.intValue), rawValue)) {
                    ui.setStatus("Memory updated");
                } else {
                    ui.setStatus("Search result not found");
                }
                syncRamSearchToUI();
                break;
            }

            case AppUI::ActionType::FreezeRamSearchResult: {
                u32 rawValue = 0;
                if (!parseWriteValue(action.text, genesis.getRamSearchValueType(), rawValue)) {
                    ui.setStatus("Enter a valid value for the selected type");
                    break;
                }
                std::string errorText;
                if (action.intValue >= 0 &&
                    genesis.freezeRamSearchResult(static_cast<size_t>(action.intValue),
                                                 rawValue,
                                                 action.text2,
                                                 &errorText)) {
                    ui.setStatus("Freeze cheat created");
                    syncCheatsToUI();
                } else {
                    ui.setStatus(errorText.empty() ? "Failed to create cheat" : errorText);
                }
                syncRamSearchToUI();
                break;
            }

            case AppUI::ActionType::RefreshRamSearchResult: {
                u32 currentValue = 0;
                if (action.intValue >= 0 &&
                    genesis.refreshRamSearchResultValue(static_cast<size_t>(action.intValue), currentValue)) {
                    ui.setStatus("Search result refreshed");
                } else {
                    ui.setStatus("Search result not found");
                }
                syncRamSearchToUI();
                break;
            }

            case AppUI::ActionType::SaveState:
                if (genesis.saveState(action.intValue, genesis.getFramebuffer())) {
                    ui.setStatus("Saved slot " + std::to_string(action.intValue));
                    ui.setSlotInfo(action.intValue, true, genesis.getFramebuffer());
                } else {
                    ui.setStatus("Save failed");
                }
                break;

            case AppUI::ActionType::LoadState:
                if (genesis.loadState(action.intValue)) {
                    genesis.resetRamSearch();
                    ui.setStatus("Loaded slot " + std::to_string(action.intValue));
                    if (audioStream) SDL_ClearAudioStream(audioStream);
                    if (menuPauseEngaged) {
                        genesis.setPaused(pauseWasActiveBeforeMenu);
                        menuPauseEngaged = false;
                    }
                    syncRamSearchToUI();
                } else {
                    ui.setStatus("Slot " + std::to_string(action.intValue) + " is empty");
                }
                break;

            case AppUI::ActionType::SetKeyBinding:
                // Key already saved by AppUI
                break;

            case AppUI::ActionType::SetFullscreenMode:
                fullscreenMode = static_cast<FullscreenMode>(action.intValue);
                if (!applyPresentationMode(window, fullscreenMode, windowScale, fullscreenResolutions,
                                           fullscreenResolutionIndex, fullscreenWidth,
                                           fullscreenHeight, fullscreenRefreshHz)) {
                    ui.setStatus("Failed to switch display mode");
                    fullscreenMode = activeFullscreenMode(window);
                    refreshFullscreenState();
                } else {
                    refreshFullscreenState();
                }
                break;

            case AppUI::ActionType::SetFullscreenResolution:
                if (action.intValue >= 0 &&
                    action.intValue < static_cast<int>(fullscreenResolutions.size())) {
                    fullscreenResolutionIndex = action.intValue;
                    fullscreenWidth = fullscreenResolutions[action.intValue].width;
                    fullscreenHeight = fullscreenResolutions[action.intValue].height;
                    fullscreenRefreshHz = fullscreenResolutions[action.intValue].refreshHz;
                    if (fullscreenMode == FullscreenMode::Exclusive) {
                        if (!applyPresentationMode(window, fullscreenMode, windowScale, fullscreenResolutions,
                                                   fullscreenResolutionIndex, fullscreenWidth,
                                                   fullscreenHeight, fullscreenRefreshHz)) {
                            ui.setStatus("Failed to switch fullscreen resolution");
                            fullscreenMode = activeFullscreenMode(window);
                            refreshFullscreenState();
                        } else {
                            refreshFullscreenState();
                        }
                    }
                }
                break;

            case AppUI::ActionType::SetScalingMode:
                scalingMode = static_cast<ScalingMode>(action.intValue);
                break;

            case AppUI::ActionType::SetAspectRatio:
                aspectRatio = static_cast<AspectRatio>(action.intValue);
                break;

            case AppUI::ActionType::SetVideoStandard:
                videoStandardMode = static_cast<VideoStandardMode>(action.intValue);
                genesis.setVideoStandard(resolveVideoStandard(videoStandardMode, genesis.getCartridge()));
                genesis.reset();
                genesis.setVideoStandard(resolveVideoStandard(videoStandardMode, genesis.getCartridge()));
                if (audioStream) SDL_ClearAudioStream(audioStream);
                refreshTimingState();
                ui.setStatus(genesis.getVideoStandard() == VideoStandard::PAL
                                 ? "Switched to PAL timing"
                                 : "Switched to NTSC timing");
                break;

            case AppUI::ActionType::SetVSync:
                vsyncEnabled = action.boolValue;
#ifndef USE_VULKAN
                SDL_GL_SetSwapInterval(action.boolValue ? 1 : 0);
#endif
                // Vulkan VSync requires swapchain recreation (not implemented yet)
                break;

            case AppUI::ActionType::SetBilinearFiltering:
                bilinearFiltering = action.boolValue;
                gameRenderer.setFilterMode(action.boolValue);
                break;

            case AppUI::ActionType::SetAutoLoadState:
                autoLoadState = action.boolValue;
                break;

            case AppUI::ActionType::CloseMenu:
                if (menuPauseEngaged) {
                    genesis.setPaused(pauseWasActiveBeforeMenu);
                    menuPauseEngaged = false;
                }
                break;
        }

        // Persist settings on any settings-related action
        switch (action.type) {
            case AppUI::ActionType::SetFrameLimiter:
            case AppUI::ActionType::SetWindowScale:
            case AppUI::ActionType::SetAudioQueueMs:
            case AppUI::ActionType::ToggleFPS:
            case AppUI::ActionType::SetProfilerMode:
            case AppUI::ActionType::SetVideoStandard:
            case AppUI::ActionType::SetFullscreenMode:
            case AppUI::ActionType::SetFullscreenResolution:
            case AppUI::ActionType::SetScalingMode:
            case AppUI::ActionType::SetAspectRatio:
            case AppUI::ActionType::SetVSync:
            case AppUI::ActionType::SetBilinearFiltering:
            case AppUI::ActionType::SetAutoLoadState:
                saveConfig(windowScale, scalingMode, aspectRatio, vsyncEnabled,
                           bilinearFiltering, showFPS, profilerMode, videoStandardMode, frameLimiterEnabled, audioQueueMs,
                           fullscreenMode, fullscreenWidth, fullscreenHeight, fullscreenRefreshHz, autoLoadState);
                break;
            default:
                break;
        }
    };

    printf("Starting emulation...\n");
    const int maxFrames = []() {
        const char* env = std::getenv("GENESIS_MAX_FRAMES");
        if (!env) return 0;
        int n = std::atoi(env);
        return n > 0 ? n : 0;
    }();
    const int startFrameCount = genesis.getFrameCount();

    while (running) {
        // Max frames for testing (relative to start, so save states work)
        if (maxFrames > 0 && (genesis.getFrameCount() - startFrameCount) >= maxFrames) {
            printf("Reached GENESIS_MAX_FRAMES=%d, stopping.\n", maxFrames);
            running = false;
            break;
        }

        const u64 frameStartTicks = SDL_GetPerformanceCounter();
        u64 sdlTicks = 0;
        u64 emulationTicks = 0;
        u64 audioTicks = 0;
        u64 videoTicks = 0;
        u64 uiTicks = 0;
        u64 presentTicks = 0;
        u64 pacingTicks = 0;

        // ---------- Event handling ----------
        SDL_Event event;
        const u64 sdlStartTicks = SDL_GetPerformanceCounter();
        while (SDL_PollEvent(&event)) {
            ui.processEvent(event);

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;

                case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                case SDL_EVENT_WINDOW_MOVED:
                case SDL_EVENT_WINDOW_RESIZED:
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    refreshFullscreenState();
#ifdef USE_VULKAN
                    if (event.type == SDL_EVENT_WINDOW_RESIZED ||
                        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                        int w = 0;
                        int h = 0;
                        SDL_GetWindowSizeInPixels(window, &w, &h);
                        if (w > 0 && h > 0) {
                            g_vkCtx.recreateSwapchain(w, h);
                        }
                    }
#endif
                    break;

                case SDL_EVENT_KEY_DOWN: {
                    SDL_Keycode key = event.key.key;

                    // Escape always toggles menu, never swallowed by ImGui
                    if (key == keyBindings.get(BindableAction::OpenMenu)) {
                        if (ui.isVisible()) {
                            ui.setVisible(false);
                            if (menuPauseEngaged) {
                                genesis.setPaused(pauseWasActiveBeforeMenu);
                                menuPauseEngaged = false;
                            }
                        } else {
                            pauseWasActiveBeforeMenu = genesis.isPaused();
                            genesis.setPaused(true);
                            menuPauseEngaged = true;
                            fullscreenMode = activeFullscreenMode(window);
                            refreshFullscreenState();
                            ui.setFrameLimiter(frameLimiterEnabled);
                            ui.setWindowScale(windowScale);
                            ui.setAudioQueueMs(audioQueueMs);
                            ui.setShowFPS(showFPS);
                            ui.setProfilerMode(profilerMode);
                            ui.setFullscreenMode(fullscreenMode);
                            ui.setScalingMode(scalingMode);
                            ui.setAspectRatio(aspectRatio);
                            ui.setVideoStandardMode(videoStandardMode);
                            ui.setEffectiveVideoTiming(genesis.getVideoStandard(),
                                                       genesis.getFrameRate(),
                                                       genesis.getScanlinesPerFrame());
                            ui.setVSync(vsyncEnabled);
                            ui.setBilinearFiltering(bilinearFiltering);
                            syncCheatsToUI();
                            // Populate save state slot info
                            for (int s = 0; s < 10; s++) {
                                if (genesis.hasState(s)) {
                                    u32 thumb[320 * 224];
                                    if (genesis.loadStateThumbnail(s, thumb)) {
                                        ui.setSlotInfo(s, true, thumb);
                                    } else {
                                        ui.setSlotInfo(s, true);
                                    }
                                } else {
                                    ui.setSlotInfo(s, false);
                                }
                            }
                            ui.setVisible(true);
                            releaseAllButtons(genesis);
                            if (audioStream) SDL_ClearAudioStream(audioStream);
                        }
                        break;
                    }

                    // If UI is waiting for a key rebind, feed it directly
                    if (ui.isListeningForKey()) {
                        ui.feedKeyForRebind(key);
                        break;
                    }

                    // If menu is open and ImGui wants keyboard, let it handle input
                    if (ui.isVisible() && ui.wantCaptureKeyboard()) {
                        break;
                    }

                    // Emulator hotkeys (when menu closed)
                    if (!ui.isVisible()) {
                        if (key == keyBindings.get(BindableAction::Quit)) {
                            running = false;
                        } else if (key == keyBindings.get(BindableAction::Pause)) {
                            genesis.setPaused(!genesis.isPaused());
                        } else if (key == keyBindings.get(BindableAction::FrameStep) && genesis.isPaused()) {
                            genesis.setPaused(false);
                            genesis.runFrame();
                            genesis.setPaused(true);
                        } else if (key == keyBindings.get(BindableAction::SingleStep) && genesis.isPaused()) {
                            genesis.step();
                        } else if (key == keyBindings.get(BindableAction::FastForward)) {
                            fastForwardHeld = true;
                            if (audioStream) SDL_ClearAudioStream(audioStream);
                        } else if (key == keyBindings.get(BindableAction::ToggleFrameLimiter)) {
                            frameLimiterEnabled = !frameLimiterEnabled;
                            if (frameLimiterEnabled) {
                                nextFrameTick = SDL_GetPerformanceCounter() + frameTicks;
                            }
                        } else if (key == keyBindings.get(BindableAction::QuickSave)) {
                            if (genesis.saveState(currentSaveSlot, genesis.getFramebuffer())) {
                                ui.setStatus("State saved to slot " + std::to_string(currentSaveSlot));
                                ui.setSlotInfo(currentSaveSlot, true, genesis.getFramebuffer());
                            } else {
                                ui.setStatus("Failed to save state");
                            }
                        } else if (key == keyBindings.get(BindableAction::QuickLoad)) {
                            if (genesis.loadState(currentSaveSlot)) {
                                ui.setStatus("State loaded from slot " + std::to_string(currentSaveSlot));
                                if (audioStream) SDL_ClearAudioStream(audioStream);
                            } else {
                                ui.setStatus("No save state in slot " + std::to_string(currentSaveSlot));
                            }
                        } else if (key == keyBindings.get(BindableAction::SaveStatePicker)) {
                            pauseWasActiveBeforeMenu = genesis.isPaused();
                            genesis.setPaused(true);
                            menuPauseEngaged = true;
                            for (int s = 0; s < 10; s++) {
                                if (genesis.hasState(s)) {
                                    u32 thumb[320 * 224];
                                    if (genesis.loadStateThumbnail(s, thumb)) {
                                        ui.setSlotInfo(s, true, thumb);
                                    } else {
                                        ui.setSlotInfo(s, true);
                                    }
                                } else {
                                    ui.setSlotInfo(s, false);
                                }
                            }
                            ui.openSaveStates();
                            releaseAllButtons(genesis);
                            if (audioStream) SDL_ClearAudioStream(audioStream);
                        } else if (key == keyBindings.get(BindableAction::NextSlot)) {
                            currentSaveSlot = (currentSaveSlot + 1) % 10;
                            ui.setStatus("Slot " + std::to_string(currentSaveSlot) + " selected");
                        } else if (key == keyBindings.get(BindableAction::PrevSlot)) {
                            currentSaveSlot = (currentSaveSlot + 9) % 10;
                            ui.setStatus("Slot " + std::to_string(currentSaveSlot) + " selected");
                        } else if (key == keyBindings.get(BindableAction::ToggleFPS)) {
                            showFPS = !showFPS;
                            genesis.setDetailedProfiling(showFPS && profilerMode == ProfilerMode::Detailed);
                        } else {
                            handleKeyEvent(genesis, key, true, keyBindings);
                        }
                    }
                    break;
                }

                case SDL_EVENT_KEY_UP:
                    if (event.key.key == keyBindings.get(BindableAction::FastForward)) {
                        fastForwardHeld = false;
                        if (frameLimiterEnabled) {
                            nextFrameTick = SDL_GetPerformanceCounter() + frameTicks;
                        }
                    }
                    if (!ui.isVisible()) {
                        handleKeyEvent(genesis, event.key.key, false, keyBindings);
                    }
                    break;

                // ---------- Gamepad events ----------
                case SDL_EVENT_GAMEPAD_ADDED:
                    openGamepad(event.gdevice.which);
                    break;

                case SDL_EVENT_GAMEPAD_REMOVED:
                    closeGamepad(event.gdevice.which);
                    break;

                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                    int port = gamepadPort(event.gbutton.which);
                    if (port < 0) break;
                    bool pressed = (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                    auto btn = static_cast<SDL_GamepadButton>(event.gbutton.button);

                    // Guide / Back (View) button toggles menu
                    if ((btn == SDL_GAMEPAD_BUTTON_GUIDE || btn == SDL_GAMEPAD_BUTTON_BACK) && pressed) {
                        if (ui.isVisible()) {
                            ui.setVisible(false);
                            if (menuPauseEngaged) {
                                genesis.setPaused(pauseWasActiveBeforeMenu);
                                menuPauseEngaged = false;
                            }
                        } else {
                            pauseWasActiveBeforeMenu = genesis.isPaused();
                            genesis.setPaused(true);
                            menuPauseEngaged = true;
                            fullscreenMode = activeFullscreenMode(window);
                            refreshFullscreenState();
                            ui.setFrameLimiter(frameLimiterEnabled);
                            ui.setWindowScale(windowScale);
                            ui.setAudioQueueMs(audioQueueMs);
                            ui.setShowFPS(showFPS);
                            ui.setProfilerMode(profilerMode);
                            ui.setFullscreenMode(fullscreenMode);
                            ui.setScalingMode(scalingMode);
                            ui.setAspectRatio(aspectRatio);
                            ui.setVideoStandardMode(videoStandardMode);
                            ui.setEffectiveVideoTiming(genesis.getVideoStandard(),
                                                       genesis.getFrameRate(),
                                                       genesis.getScanlinesPerFrame());
                            ui.setVSync(vsyncEnabled);
                            ui.setBilinearFiltering(bilinearFiltering);
                            syncCheatsToUI();
                            for (int s = 0; s < 10; s++) {
                                if (genesis.hasState(s)) {
                                    u32 thumb[320 * 224];
                                    if (genesis.loadStateThumbnail(s, thumb)) {
                                        ui.setSlotInfo(s, true, thumb);
                                    } else {
                                        ui.setSlotInfo(s, true);
                                    }
                                } else {
                                    ui.setSlotInfo(s, false);
                                }
                            }
                            ui.setVisible(true);
                            releaseAllButtons(genesis);
                            if (audioStream) SDL_ClearAudioStream(audioStream);
                        }
                        break;
                    }

                    if (!ui.isVisible()) {
                        handleGamepadButton(genesis, port, btn, pressed);
                    }
                    break;
                }

                case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                    int port = gamepadPort(event.gaxis.which);
                    if (port < 0) break;
                    if (!ui.isVisible()) {
                        handleGamepadAxis(genesis, port,
                            static_cast<SDL_GamepadAxis>(event.gaxis.axis),
                            event.gaxis.value);
                    }
                    break;
                }
            }
        }
        sdlTicks += SDL_GetPerformanceCounter() - sdlStartTicks;

        // ---------- Run emulation ----------
        if (!ui.isVisible() && romLoaded) {
            const u64 emulationStartTicks = SDL_GetPerformanceCounter();
            // During fast-forward, run multiple frames per render to overcome
            // the vsync bottleneck. Only the last frame's video output matters.
            int framesToRun = fastForwardHeld ? fastForwardMultiplier : 1;
            for (int ff = 0; ff < framesToRun; ff++) {
                genesis.runFrame();
            }
            emulationTicks += SDL_GetPerformanceCounter() - emulationStartTicks;
        }

        // ---------- Audio ----------
        if (!ui.isVisible() && romLoaded && audioStream &&
            genesis.getAudioSamples() > 0 && !fastForwardHeld) {
            const u64 audioStartTicks = SDL_GetPerformanceCounter();
            const int queued = SDL_GetAudioStreamAvailable(audioStream);
            const u32 maxQueuedBytes = (audioBytesPerSecond * static_cast<u32>(audioQueueMs)) / 1000;
            if (queued > static_cast<int>(maxQueuedBytes)) {
                SDL_ClearAudioStream(audioStream);
            }
            SDL_PutAudioStreamData(audioStream, genesis.getAudioBuffer(),
                                   genesis.getAudioSamples() * 2 * sizeof(s16));
            audioTicks += SDL_GetPerformanceCounter() - audioStartTicks;
        }

        // ---------- FPS counter ----------
        fpsCount++;
        u64 fpsNow = SDL_GetPerformanceCounter();
        if (fpsNow - fpsTimer >= perfFreq) {
            displayFPS = fpsCount;
            fpsCount = 0;
            fpsTimer = fpsNow;
        }

        // ---------- Render ----------
#ifdef USE_VULKAN
        {
            const u64 beginFrameTicks = SDL_GetPerformanceCounter();
            VkCommandBuffer cmd = g_vkCtx.beginFrame();
            presentTicks += SDL_GetPerformanceCounter() - beginFrameTicks;
            if (cmd == VK_NULL_HANDLE) continue; // swapchain recreated, skip

            const u64 videoStartTicks = SDL_GetPerformanceCounter();
            int drawW = g_vkCtx.windowData()->Width;
            int drawH = g_vkCtx.windowData()->Height;

            // Resize texture if interlace mode changed dimensions
            gameRenderer.resize(genesis.getScreenWidth(), genesis.getScreenHeight());
            gameRenderer.upload(genesis.getFramebuffer());
            gameRenderer.draw(drawW, drawH, scalingMode, aspectRatio);
            videoTicks += SDL_GetPerformanceCounter() - videoStartTicks;

            // ImGui frame
            const u64 uiStartTicks = SDL_GetPerformanceCounter();
            ui.newFrame();

            if (ui.isVisible()) {
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                bg->AddRectFilled(ImVec2(0, 0),
                                  ImGui::GetIO().DisplaySize,
                                  IM_COL32(0, 0, 0, 128));
            }

            AppUI::Action action = ui.draw();
            if (action.type != AppUI::ActionType::None) {
                applyAction(action);
            }

            if (showFPS) {
                ui.drawFPSOverlay(profilerStats, profilerMode);
            }
            ui.drawStatusOverlay();

            ui.render(); // calls ImGui::Render()
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            uiTicks += SDL_GetPerformanceCounter() - uiStartTicks;

            const u64 endFrameTicks = SDL_GetPerformanceCounter();
            g_vkCtx.endFrame(cmd);
            presentTicks += SDL_GetPerformanceCounter() - endFrameTicks;
        }
#else
        {
            const u64 videoStartTicks = SDL_GetPerformanceCounter();
            // Get actual pixel dimensions (HiDPI-aware)
            int drawW, drawH;
            SDL_GetWindowSizeInPixels(window, &drawW, &drawH);

            glViewport(0, 0, drawW, drawH);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            // Resize texture if interlace mode changed dimensions
            gameRenderer.resize(genesis.getScreenWidth(), genesis.getScreenHeight());
            gameRenderer.upload(genesis.getFramebuffer());
            gameRenderer.draw(drawW, drawH, scalingMode, aspectRatio);
            videoTicks += SDL_GetPerformanceCounter() - videoStartTicks;

            // ImGui frame
            const u64 uiStartTicks = SDL_GetPerformanceCounter();
            ui.newFrame();

            // Dim game when menu is open (drawn behind all ImGui windows)
            if (ui.isVisible()) {
                ImDrawList* bg = ImGui::GetBackgroundDrawList();
                bg->AddRectFilled(ImVec2(0, 0),
                                  ImGui::GetIO().DisplaySize,
                                  IM_COL32(0, 0, 0, 128));
            }

            AppUI::Action action = ui.draw();
            if (action.type != AppUI::ActionType::None) {
                applyAction(action);
            }

            if (showFPS) {
                ui.drawFPSOverlay(profilerStats, profilerMode);
            }
            ui.drawStatusOverlay();

            ui.render();
            uiTicks += SDL_GetPerformanceCounter() - uiStartTicks;

            const u64 swapStartTicks = SDL_GetPerformanceCounter();
            SDL_GL_SwapWindow(window);
            presentTicks += SDL_GetPerformanceCounter() - swapStartTicks;
        }
#endif

        // ---------- Frame pacing ----------
        if (frameLimiterEnabled && !fastForwardHeld) {
            const u64 pacingStartTicks = SDL_GetPerformanceCounter();
            u64 now = SDL_GetPerformanceCounter();
            if (now < nextFrameTick) {
                double msRemaining = (static_cast<double>(nextFrameTick - now) * 1000.0) /
                                     static_cast<double>(perfFreq);
                if (msRemaining > 1.0) {
                    SDL_Delay(static_cast<u32>(msRemaining - 0.5));
                }
                while (SDL_GetPerformanceCounter() < nextFrameTick) {
                    // short spin for sub-millisecond precision
                }
                nextFrameTick += frameTicks;
            } else {
                nextFrameTick = now + frameTicks;
            }
            pacingTicks += SDL_GetPerformanceCounter() - pacingStartTicks;
        }

        const u64 frameEndTicks = SDL_GetPerformanceCounter();
        AppUI::PerformanceStats sampleStats{};
        sampleStats.fps = displayFPS;
        sampleStats.frameMs = ticksToMs(frameEndTicks - frameStartTicks, perfFreq);
        sampleStats.sdlMs = ticksToMs(sdlTicks, perfFreq);
        sampleStats.emulationMs = ticksToMs(emulationTicks, perfFreq);
        Genesis::FrameProfile frameProfile{};
        if (sampleStats.emulationMs > 0.0f) {
            frameProfile = genesis.getFrameProfile();
        }
        sampleStats.cheatsMs = static_cast<float>(frameProfile.cheatsMs);
        sampleStats.m68kMs = static_cast<float>(frameProfile.m68kMs);
        sampleStats.z80Ms = static_cast<float>(frameProfile.z80Ms);
        sampleStats.vdpMs = static_cast<float>(frameProfile.vdpMs);
        sampleStats.ymMs = static_cast<float>(frameProfile.ymMs);
        sampleStats.psgMs = static_cast<float>(frameProfile.psgMs);
        sampleStats.mixMs = static_cast<float>(frameProfile.mixMs);
        const float trackedEmulationMs = sampleStats.cheatsMs +
                                         sampleStats.m68kMs +
                                         sampleStats.z80Ms +
                                         sampleStats.vdpMs +
                                         sampleStats.ymMs +
                                         sampleStats.psgMs +
                                         sampleStats.mixMs;
        sampleStats.emulationOtherMs = std::max(0.0f, sampleStats.emulationMs - trackedEmulationMs);
        sampleStats.audioMs = ticksToMs(audioTicks, perfFreq);
        sampleStats.videoMs = ticksToMs(videoTicks, perfFreq);
        sampleStats.uiMs = ticksToMs(uiTicks, perfFreq);
        sampleStats.presentMs = ticksToMs(presentTicks, perfFreq);
        sampleStats.pacingMs = ticksToMs(pacingTicks, perfFreq);

        const float trackedBusyMs = sampleStats.sdlMs +
                                    sampleStats.emulationMs +
                                    sampleStats.audioMs +
                                    sampleStats.videoMs +
                                    sampleStats.uiMs +
                                    sampleStats.presentMs;
        sampleStats.busyMs = std::max(0.0f, sampleStats.frameMs - sampleStats.pacingMs);
        sampleStats.otherMs = std::max(0.0f, sampleStats.busyMs - trackedBusyMs);
        smoothProfilerStats(sampleStats);
    }

    printf("Emulation stopped after %d frames\n", genesis.getFrameCount());

    // ---------- Cleanup ----------
    ui.shutdown();
    gameRenderer.shutdown();

    if (audioStream) {
        SDL_DestroyAudioStream(audioStream);
    }

#ifdef USE_VULKAN
    ImGui_ImplVulkan_Shutdown();
    g_vkCtx.shutdown();
#else
    SDL_GL_DestroyContext(glContext);
#endif
    for (int i = 0; i < 2; i++) {
        if (gamepads[i]) { SDL_CloseGamepad(gamepads[i]); gamepads[i] = nullptr; }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
