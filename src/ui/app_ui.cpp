// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/app_ui.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#ifdef USE_VULKAN
#include "imgui_impl_vulkan.h"
#else
#include "imgui_impl_opengl3.h"
#endif

#include <SDL3/SDL.h>
#ifdef USE_VULKAN
#include <SDL3/SDL_vulkan.h>
#endif
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef USE_VULKAN
#include <vulkan/vulkan.h>
#else
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif
#endif

// -----------------------------------------------------------------------
// Semantic color constants
// -----------------------------------------------------------------------

namespace UIColors {
    static const ImVec4 Success       = ImVec4(0.30f, 0.80f, 0.40f, 1.00f);
    static const ImVec4 Warning       = ImVec4(0.95f, 0.75f, 0.20f, 1.00f);
    static const ImVec4 Error         = ImVec4(0.90f, 0.30f, 0.30f, 1.00f);
    static const ImVec4 Info          = ImVec4(0.40f, 0.65f, 1.00f, 1.00f);
    static const ImVec4 Accent        = ImVec4(0.40f, 0.50f, 1.00f, 1.00f);
    static const ImVec4 AccentTeal    = ImVec4(0.10f, 0.70f, 0.75f, 1.00f);
    static const ImVec4 AccentAmber   = ImVec4(0.95f, 0.75f, 0.30f, 1.00f);
    static const ImVec4 Subtle        = ImVec4(0.35f, 0.40f, 0.55f, 0.50f);
    static const ImVec4 Danger        = ImVec4(0.80f, 0.20f, 0.20f, 1.00f);
    static const ImVec4 DangerHov     = ImVec4(0.90f, 0.30f, 0.30f, 1.00f);
    static const ImVec4 DangerAct     = ImVec4(1.00f, 0.35f, 0.35f, 1.00f);
    static const ImVec4 SlotUsed      = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    static const ImVec4 SlotEmpty     = ImVec4(0.30f, 0.30f, 0.40f, 0.60f);
}

// -----------------------------------------------------------------------
// Custom toggle switch widget
// -----------------------------------------------------------------------

static bool ToggleSwitch(const char* str_id, bool* v) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(str_id);

    const float height = ImGui::GetFrameHeight();
    const float width = height * 1.8f;
    const float radius = height * 0.5f;

    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *v = !*v;
        ImGui::MarkItemEdited(id);
    }

    float t = *v ? 1.0f : 0.0f;
    // Animate
    if (g.LastActiveId == id) {
        float t_anim = ImSaturate(g.LastActiveIdTimer / 0.10f);
        t = *v ? t_anim : 1.0f - t_anim;
    }

    // Track color
    ImU32 col_bg;
    if (*v) {
        col_bg = ImGui::GetColorU32(hovered ? ImVec4(0.25f, 0.72f, 0.35f, 1.0f) : ImVec4(0.20f, 0.65f, 0.30f, 1.0f));
    } else {
        col_bg = ImGui::GetColorU32(hovered ? ImVec4(0.40f, 0.40f, 0.50f, 1.0f) : ImVec4(0.30f, 0.30f, 0.40f, 1.0f));
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);

    // Knob
    float knob_x = bb.Min.x + radius + t * (width - height);
    float knob_y = bb.Min.y + radius;
    draw_list->AddCircleFilled(ImVec2(knob_x, knob_y), radius - 2.0f, IM_COL32(255, 255, 255, 255));

    return pressed;
}

static const char* fullscreenModeLabel(FullscreenMode mode) {
    switch (mode) {
        case FullscreenMode::Windowed:          return "Windowed";
        case FullscreenMode::BorderlessDesktop: return "Borderless Fullscreen";
        case FullscreenMode::Exclusive:         return "Exclusive Fullscreen";
    }
    return "Windowed";
}

static constexpr float UI_REFERENCE_WIDTH = 960.0f;
static constexpr float UI_REFERENCE_HEIGHT = 720.0f;

static bool beginSettingsCard(const char* id, const char* title, const char* subtitle = nullptr) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, ImGui::GetStyle().ChildRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.18f, 0.86f));
    if (!ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                           ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return false;
    }

    ImGui::TextUnformatted(title);
    if (subtitle && subtitle[0] != '\0') {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", subtitle);
        ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    return true;
}

static void endSettingsCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// -----------------------------------------------------------------------
// Init / Shutdown
// -----------------------------------------------------------------------

void AppUI::init(SDL_Window* window, void* backendContext) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    window_ = window;

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // don't save layout
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Load scalable vector default font
    io.Fonts->AddFontDefaultVector();

    updateScaleMetrics(true);

#ifdef USE_VULKAN
    ImGui_ImplSDL3_InitForVulkan(window);
    // Vulkan ImGui init is completed by main.cpp after VulkanContext is ready
    (void)backendContext;
#else
    ImGui_ImplSDL3_InitForOpenGL(window, backendContext);
    ImGui_ImplOpenGL3_Init("#version 150");
#endif

    loadRecentRoms();
}

void AppUI::shutdown() {
    // Clean up slot thumbnails
    for (auto& si : slotInfo_) {
        if (si.glTexture) {
#ifndef USE_VULKAN
            glDeleteTextures(1, &si.glTexture);
#endif
            si.glTexture = 0;
        }
    }

#ifdef USE_VULKAN
    ImGui_ImplVulkan_Shutdown();
#else
    ImGui_ImplOpenGL3_Shutdown();
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

// -----------------------------------------------------------------------
// Frame cycle
// -----------------------------------------------------------------------

bool AppUI::processEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    return false; // let main loop also see it
}

void AppUI::newFrame() {
    updateScaleMetrics();
#ifdef USE_VULKAN
    ImGui_ImplVulkan_NewFrame();
#else
    ImGui_ImplOpenGL3_NewFrame();
#endif
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

AppUI::Action AppUI::draw() {
    pending_ = {};

    if (visible_) {
        drawMenuBar();
        if (showFileBrowser_)   drawFileBrowser();
        if (showSaveStates_)    drawSaveStates();
        if (showCheatManager_)  drawCheatManager();
        if (showVideoSettings_) drawVideoSettings();
        if (showAudioSettings_) drawAudioSettings();
        if (showEmuSettings_)   drawEmuSettings();
        if (showInputSettings_) drawInputSettings();
        if (showAbout_)         drawAbout();
        if (showControls_)      drawControls();
    }

    return pending_;
}

void AppUI::render() {
    ImGui::Render();
#ifndef USE_VULKAN
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
    // For Vulkan, the caller (main.cpp) calls
    // ImGui_ImplVulkan_RenderDrawData() with the active command buffer.
}

// -----------------------------------------------------------------------
// State helpers
// -----------------------------------------------------------------------

bool AppUI::wantCaptureKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

bool AppUI::wantCaptureMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

void AppUI::setCurrentRomPath(const std::string& p) {
    std::filesystem::path romPath(p);
    if (romPath.has_parent_path()) {
        currentDir_ = romPath.parent_path();
    }
}

void AppUI::setStatus(const std::string& text) {
    statusText_ = text;
    statusTimer_ = 3.0f; // seconds
}

void AppUI::setFullscreenResolutions(const std::vector<FullscreenResolution>& modes,
                                     int selectedIndex,
                                     const std::string& displayName) {
    fullscreenResolutions_ = modes;
    fullscreenResolutionIndex_ = selectedIndex;
    fullscreenDisplayName_ = displayName;
}

void AppUI::setEffectiveVideoTiming(VideoStandard standard, double refreshHz, int scanlines) {
    effectiveVideoStandard_ = standard;
    effectiveRefreshHz_ = refreshHz;
    effectiveScanlines_ = scanlines;
}

void AppUI::updateScaleMetrics(bool force) {
    if (!window_) return;

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window_, &windowWidth, &windowHeight);

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GetWindowSizeInPixels(window_, &drawableWidth, &drawableHeight);

    if (!force &&
        windowWidth == lastWindowWidth_ &&
        windowHeight == lastWindowHeight_ &&
        drawableWidth == lastDrawableWidth_ &&
        drawableHeight == lastDrawableHeight_) {
        return;
    }

    lastWindowWidth_ = windowWidth;
    lastWindowHeight_ = windowHeight;
    lastDrawableWidth_ = drawableWidth;
    lastDrawableHeight_ = drawableHeight;

    const float contentScale = SDL_GetWindowDisplayScale(window_);
    const float framebufferScale =
        (windowWidth > 0) ? static_cast<float>(drawableWidth) / static_cast<float>(windowWidth) : 1.0f;
    physicalDpiScale_ = std::clamp(std::max(contentScale, framebufferScale), 1.0f, 3.0f);

    const float scaleX = (windowWidth > 0) ? static_cast<float>(windowWidth) / UI_REFERENCE_WIDTH : 1.0f;
    const float scaleY = (windowHeight > 0) ? static_cast<float>(windowHeight) / UI_REFERENCE_HEIGHT : 1.0f;
    layoutScale_ = std::clamp(std::min(scaleX, scaleY), 0.50f, 1.00f);
    uiScale_ = layoutScale_;

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    applyStyle();
    style.FontSizeBase = 13.0f;
    style.FontScaleDpi = 1.0f;
    style.FontScaleMain = layoutScale_;
    style.ScaleAllSizes(layoutScale_);
}

void AppUI::prepareCenteredWindow(const ImVec2& desiredSize,
                                  const ImVec2& minSize,
                                  const ImVec2& maxSize) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float marginX = scaled(28.0f);
    const float marginY = scaled(44.0f);
    const ImVec2 available(
        std::max(1.0f, viewport->WorkSize.x - marginX),
        std::max(1.0f, viewport->WorkSize.y - marginY));

    const ImVec2 constrainedMax(
        std::min(maxSize.x, available.x),
        std::min(maxSize.y, available.y));
    const ImVec2 constrainedMin(
        std::min(minSize.x, constrainedMax.x),
        std::min(minSize.y, constrainedMax.y));
    const ImVec2 constrainedDesired(
        std::clamp(desiredSize.x, constrainedMin.x, constrainedMax.x),
        std::clamp(desiredSize.y, constrainedMin.y, constrainedMax.y));

    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(constrainedDesired, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(constrainedMin, constrainedMax);
}

void AppUI::pushAction(Action a) {
    if (pending_.type == ActionType::None) {
        pending_ = a;
    }
}

void AppUI::setSlotInfo(int slot, bool occupied, const u32* thumbnail) {
    if (slot < 0 || slot >= 10) return;
    slotInfo_[slot].occupied = occupied;

#ifndef USE_VULKAN
    if (thumbnail && occupied) {
        if (!slotInfo_[slot].glTexture) {
            glGenTextures(1, &slotInfo_[slot].glTexture);
            glBindTexture(GL_TEXTURE_2D, slotInfo_[slot].glTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 224, 0,
                         GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, thumbnail);
        } else {
            glBindTexture(GL_TEXTURE_2D, slotInfo_[slot].glTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 224,
                            GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, thumbnail);
        }
    }
#else
    // TODO: Vulkan thumbnail textures via ImGui_ImplVulkan_AddTexture()
    (void)thumbnail;
#endif
}

// -----------------------------------------------------------------------
// Recent ROMs
// -----------------------------------------------------------------------

std::filesystem::path AppUI::getConfigDir() {
#ifdef _WIN32
    const char* home = std::getenv("APPDATA");
    if (!home) home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) home = ".";
    auto dir = std::filesystem::path(home) / ".genesis";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void AppUI::addRecentRom(const std::string& path) {
    // Remove if already present
    recentRoms_.erase(
        std::remove_if(recentRoms_.begin(), recentRoms_.end(),
                        [&](const RecentRom& r) { return r.path == path; }),
        recentRoms_.end());
    // Add to front
    RecentRom r;
    r.path = path;
    r.name = std::filesystem::path(path).filename().string();
    recentRoms_.insert(recentRoms_.begin(), r);
    if (recentRoms_.size() > 10) recentRoms_.resize(10);
    saveRecentRoms();
}

void AppUI::loadRecentRoms() {
    auto path = getConfigDir() / "recent_roms.txt";
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        RecentRom r;
        r.path = line;
        r.name = std::filesystem::path(line).filename().string();
        recentRoms_.push_back(r);
    }
}

void AppUI::saveRecentRoms() {
    auto path = getConfigDir() / "recent_roms.txt";
    std::ofstream out(path);
    for (const auto& r : recentRoms_) {
        out << r.path << "\n";
    }
}

// -----------------------------------------------------------------------
// Style
// -----------------------------------------------------------------------

void AppUI::applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();

    // Geometry (base values before ScaleAllSizes)
    s.WindowRounding    = 14.0f;
    s.FrameRounding     = 10.0f;
    s.GrabRounding      = 10.0f;
    s.ChildRounding     = 12.0f;
    s.PopupRounding     = 12.0f;
    s.TabRounding       = 10.0f;
    s.ScrollbarRounding = 10.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.TabBorderSize     = 0.0f;

    s.WindowPadding     = ImVec2(14.0f, 14.0f);
    s.FramePadding      = ImVec2(10.0f, 6.0f);
    s.ItemSpacing       = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    s.CellPadding       = ImVec2(8.0f, 6.0f);
    s.ScrollbarSize     = 14.0f;
    s.IndentSpacing     = 18.0f;

    s.WindowTitleAlign  = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_Right;
    s.ButtonTextAlign   = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f;
    s.SeparatorTextPadding    = ImVec2(16.0f, 6.0f);

    ImVec4* c = s.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.05f, 0.06f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.08f, 0.10f, 0.16f, 0.92f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.06f, 0.08f, 0.14f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.20f, 0.33f, 0.45f, 0.40f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.07f, 0.09f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.11f, 0.17f, 0.28f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.05f, 0.07f, 0.12f, 0.88f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.08f, 0.11f, 0.18f, 0.94f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.10f, 0.13f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.13f, 0.19f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.15f, 0.22f, 0.31f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.15f, 0.32f, 0.38f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.20f, 0.41f, 0.48f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.23f, 0.47f, 0.54f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.14f, 0.24f, 0.31f, 0.88f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.18f, 0.31f, 0.39f, 0.96f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.23f, 0.39f, 0.47f, 1.00f);

    c[ImGuiCol_Tab]                  = ImVec4(0.10f, 0.15f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.16f, 0.28f, 0.38f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.18f, 0.32f, 0.43f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.08f, 0.11f, 0.17f, 0.96f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.13f, 0.20f, 0.28f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.04f, 0.05f, 0.10f, 0.70f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.22f, 0.30f, 0.38f, 0.95f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.27f, 0.39f, 0.49f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.30f, 0.45f, 0.54f, 1.00f);

    c[ImGuiCol_SliderGrab]           = ImVec4(0.82f, 0.69f, 0.32f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.90f, 0.77f, 0.36f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.90f, 0.76f, 0.34f, 1.00f);

    c[ImGuiCol_Separator]            = ImVec4(0.19f, 0.29f, 0.39f, 0.55f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.24f, 0.38f, 0.50f, 0.75f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.28f, 0.45f, 0.58f, 0.92f);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.16f, 0.28f, 0.38f, 0.40f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.22f, 0.41f, 0.52f, 0.78f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.26f, 0.50f, 0.62f, 0.95f);

    c[ImGuiCol_Text]                 = ImVec4(0.94f, 0.95f, 0.98f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.53f, 0.58f, 0.67f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.24f, 0.47f, 0.62f, 0.35f);

    c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.08f, 0.10f, 0.16f, 0.50f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.01f, 0.02f, 0.05f, 0.70f);
}

// -----------------------------------------------------------------------
// FPS overlay (drawn even when menu hidden)
// -----------------------------------------------------------------------

void AppUI::drawFPSOverlay(const PerformanceStats& stats, ProfilerMode mode) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(scaled(8), scaled(8)));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(scaled(10), scaled(8)));
    if (ImGui::Begin("##fps", nullptr, flags)) {
        ImVec4 fpsColor;
        if (stats.fps >= 55)
            fpsColor = UIColors::Success;
        else if (stats.fps >= 45)
            fpsColor = UIColors::Warning;
        else
            fpsColor = UIColors::Error;

        const float busyMs = std::max(stats.busyMs, 0.0001f);
        const float frameMs = std::max(stats.frameMs, 0.0001f);
        const float busyPctOfFrame = std::clamp((stats.busyMs / frameMs) * 100.0f, 0.0f, 100.0f);

        ImGui::TextColored(fpsColor, "FPS %d", stats.fps);
        ImGui::SameLine();
        ImGui::TextDisabled("| %.2f ms frame", stats.frameMs);
        ImGui::Text("Busy %.2f ms (%.0f%% of frame)", stats.busyMs, busyPctOfFrame);

        enum class PercentBase {
            Busy,
            Emulation,
            Frame,
        };

        struct Row {
            const char* label;
            float ms;
            ImVec4 color;
            PercentBase base;
        };

        const float emuMs = std::max(stats.emulationMs, 0.0001f);
        const Row simpleRows[] = {
            {"Emu",        stats.emulationMs, UIColors::AccentAmber, PercentBase::Busy},
            {"Render",     stats.videoMs,     UIColors::AccentTeal,  PercentBase::Busy},
            {"Swap",       stats.presentMs,   UIColors::Info,        PercentBase::Busy},
            {"UI",         stats.uiMs,        UIColors::Accent,      PercentBase::Busy},
            {"SDL",        stats.sdlMs,       UIColors::Success,     PercentBase::Busy},
            {"Audio I/O",  stats.audioMs,     UIColors::Warning,     PercentBase::Busy},
            {"Other",      stats.otherMs,     ImVec4(0.68f, 0.64f, 0.84f, 1.00f), PercentBase::Busy},
            {"Idle",       stats.pacingMs,    ImVec4(0.42f, 0.48f, 0.58f, 1.00f), PercentBase::Frame},
        };
        const Row detailedRows[] = {
            {"Emu",         stats.emulationMs,      UIColors::AccentAmber, PercentBase::Busy},
            {"  68K",       stats.m68kMs,           UIColors::AccentAmber, PercentBase::Emulation},
            {"  Z80",       stats.z80Ms,            UIColors::Warning,     PercentBase::Emulation},
            {"  VDP",       stats.vdpMs,            UIColors::AccentTeal,  PercentBase::Emulation},
            {"  YM2612",    stats.ymMs,             UIColors::Success,     PercentBase::Emulation},
            {"  PSG",       stats.psgMs,            UIColors::Info,        PercentBase::Emulation},
            {"  Mix",       stats.mixMs,            UIColors::Accent,      PercentBase::Emulation},
            {"  Cheats",    stats.cheatsMs,         ImVec4(0.76f, 0.67f, 0.31f, 1.00f), PercentBase::Emulation},
            {"  Emu Other", stats.emulationOtherMs, ImVec4(0.68f, 0.64f, 0.84f, 1.00f), PercentBase::Emulation},
            {"Render",      stats.videoMs,          UIColors::AccentTeal,  PercentBase::Busy},
            {"Swap",        stats.presentMs,        UIColors::Info,        PercentBase::Busy},
            {"UI",          stats.uiMs,             UIColors::Accent,      PercentBase::Busy},
            {"SDL",         stats.sdlMs,            UIColors::Success,     PercentBase::Busy},
            {"Audio I/O",   stats.audioMs,          UIColors::Warning,     PercentBase::Busy},
            {"Other",       stats.otherMs,          ImVec4(0.68f, 0.64f, 0.84f, 1.00f), PercentBase::Busy},
            {"Idle",        stats.pacingMs,         ImVec4(0.42f, 0.48f, 0.58f, 1.00f), PercentBase::Frame},
        };
        const Row* rows = (mode == ProfilerMode::Detailed) ? detailedRows : simpleRows;
        const int rowCount = (mode == ProfilerMode::Detailed)
            ? static_cast<int>(sizeof(detailedRows) / sizeof(detailedRows[0]))
            : static_cast<int>(sizeof(simpleRows) / sizeof(simpleRows[0]));

        if (ImGui::BeginTable("perf", 3, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("Subsystem", ImGuiTableColumnFlags_WidthFixed, scaled(72));
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, scaled(62));
            ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, scaled(58));

            for (int i = 0; i < rowCount; i++) {
                const Row& row = rows[i];
                float denominator = busyMs;
                const char* suffix = "";
                switch (row.base) {
                    case PercentBase::Busy:
                        denominator = busyMs;
                        break;
                    case PercentBase::Emulation:
                        denominator = emuMs;
                        suffix = " emu";
                        break;
                    case PercentBase::Frame:
                        denominator = frameMs;
                        suffix = " frame";
                        break;
                }
                const float pct = std::clamp((row.ms / denominator) * 100.0f, 0.0f, 999.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(row.color, "%s", row.label);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f ms", row.ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f%%%s", pct, suffix);
            }

            ImGui::EndTable();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// -----------------------------------------------------------------------
// Status overlay
// -----------------------------------------------------------------------

void AppUI::drawStatusOverlay() {
    if (statusTimer_ <= 0.0f || statusText_.empty()) return;
    statusTimer_ -= ImGui::GetIO().DeltaTime;

    float alpha = statusTimer_ < 1.0f ? statusTimer_ : 1.0f;
    if (alpha <= 0.0f) return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoInputs;
    ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y - scaled(40)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.6f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled(8));
    if (ImGui::Begin("##status", nullptr, flags)) {
        ImGui::TextColored(ImVec4(1, 1, 1, alpha), "%s", statusText_.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// -----------------------------------------------------------------------
// Menu bar
// -----------------------------------------------------------------------

void AppUI::drawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open ROM...")) {
                showFileBrowser_ = true;
                refreshDirectory();
            }
            if (ImGui::BeginMenu("Recent ROMs", !recentRoms_.empty())) {
                for (const auto& r : recentRoms_) {
                    if (ImGui::MenuItem(r.name.c_str())) {
                        Action a;
                        a.type = ActionType::LoadRom;
                        a.text = r.path;
                        pushAction(a);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save States...")) {
                showSaveStates_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset")) {
                Action a;
                a.type = ActionType::Reset;
                pushAction(a);
            }
            if (ImGui::MenuItem("Resume")) {
                visible_ = false;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                Action a;
                a.type = ActionType::Quit;
                pushAction(a);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Video..."))
                showVideoSettings_ = true;
            if (ImGui::MenuItem("Audio..."))
                showAudioSettings_ = true;
            if (ImGui::MenuItem("Input..."))
                showInputSettings_ = true;
            if (ImGui::MenuItem("Emulation..."))
                showEmuSettings_ = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools")) {
            if (ImGui::MenuItem("Cheat Manager..."))
                showCheatManager_ = true;
            ImGui::Separator();
            if (!romLoaded_) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Load Cheats")) {
                Action a;
                a.type = ActionType::LoadCheatsFromFile;
                pushAction(a);
            }
            if (ImGui::MenuItem("Save Cheats")) {
                Action a;
                a.type = ActionType::SaveCheatsToFile;
                pushAction(a);
            }
            if (!romLoaded_) ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Controls..."))
                showControls_ = true;
            if (ImGui::MenuItem("About..."))
                showAbout_ = true;
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

// -----------------------------------------------------------------------
// File browser
// -----------------------------------------------------------------------

void AppUI::refreshDirectory() {
    dirEntries_.clear();
    if (currentDir_.empty()) {
        currentDir_ = std::filesystem::current_path();
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(currentDir_, ec)) {
        currentDir_ = std::filesystem::current_path();
    }

    // Parent directory entry
    if (currentDir_.has_parent_path() && currentDir_ != currentDir_.root_path()) {
        dirEntries_.push_back({"..", true});
    }

    std::vector<DirEntry> dirs, files;
    for (const auto& entry : std::filesystem::directory_iterator(currentDir_, ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (entry.is_directory(ec)) {
            dirs.push_back({name, true, 0});
        } else {
            // Filter ROM files
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".bin" || ext == ".md" || ext == ".gen" || ext == ".smd") {
                std::uintmax_t sz = entry.file_size(ec);
                files.push_back({name, false, ec ? 0 : sz});
            }
        }
    }

    std::sort(dirs.begin(), dirs.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.name < b.name;
    });
    std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) {
        return a.name < b.name;
    });

    dirEntries_.insert(dirEntries_.end(), dirs.begin(), dirs.end());
    dirEntries_.insert(dirEntries_.end(), files.begin(), files.end());
}

void AppUI::drawFileBrowser() {
    prepareCenteredWindow(ImVec2(scaled(540), scaled(440)),
                          ImVec2(scaled(360), scaled(260)),
                          ImVec2(scaled(800), scaled(600)));
    if (!ImGui::Begin("Open ROM", &showFileBrowser_)) {
        ImGui::End();
        return;
    }

    // Editable path bar
    std::string dirStr = currentDir_.string();
    std::strncpy(pathBarBuf_, dirStr.c_str(), sizeof(pathBarBuf_) - 1);
    pathBarBuf_[sizeof(pathBarBuf_) - 1] = '\0';
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##pathbar", "Enter path...", pathBarBuf_, sizeof(pathBarBuf_),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::filesystem::path newDir(pathBarBuf_);
        std::error_code ec;
        if (std::filesystem::is_directory(newDir, ec)) {
            currentDir_ = newDir;
            refreshDirectory();
        }
    }
    ImGui::Separator();

    // File list table
    float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("FileList", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, scaled(80));
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < dirEntries_.size(); i++) {
                const auto& entry = dirEntries_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                std::string label;
                if (entry.isDir) {
                    label = ">> " + entry.name;
                } else {
                    label = entry.name;
                }

                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns |
                        ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (entry.isDir) {
                        if (entry.name == "..") {
                            currentDir_ = currentDir_.parent_path();
                        } else {
                            currentDir_ /= entry.name;
                        }
                        refreshDirectory();
                    } else {
                        Action a;
                        a.type = ActionType::LoadRom;
                        a.text = (currentDir_ / entry.name).string();
                        pushAction(a);
                        showFileBrowser_ = false;
                        visible_ = false;
                    }
                }
                ImGui::PopID();

                ImGui::TableNextColumn();
                if (!entry.isDir && entry.size > 0) {
                    if (entry.size >= 1024 * 1024) {
                        ImGui::Text("%.1f MB", entry.size / (1024.0 * 1024.0));
                    } else {
                        ImGui::Text("%.1f KB", entry.size / 1024.0);
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Cancel")) {
        showFileBrowser_ = false;
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Save states
// -----------------------------------------------------------------------

void AppUI::drawSaveStates() {
    prepareCenteredWindow(ImVec2(scaled(420), scaled(360)),
                          ImVec2(scaled(340), scaled(280)),
                          ImVec2(scaled(640), scaled(540)));
    if (!ImGui::Begin("Save States", &showSaveStates_)) {
        ImGui::End();
        return;
    }

    // Slot list on left, preview on right
    float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float listWidth = std::min(scaled(200), ImGui::GetContentRegionAvail().x * 0.48f);
    ImGui::BeginChild("SlotList", ImVec2(listWidth, -footerHeight), ImGuiChildFlags_Borders);
    for (int i = 0; i < 10; i++) {
        ImGui::PushID(i);

        // Draw colored dot indicator
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float dotRadius = 4.0f * uiScale_;
        float lineH = ImGui::GetTextLineHeight();
        ImVec2 dotCenter(pos.x + dotRadius + 2.0f, pos.y + lineH * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 dotColor = slotInfo_[i].occupied ? UIColors::SlotUsed : UIColors::SlotEmpty;
        dl->AddCircleFilled(dotCenter, dotRadius, ImGui::GetColorU32(dotColor));

        // Offset text past the dot
        float indent = dotRadius * 2.0f + 8.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

        char label[32];
        snprintf(label, sizeof(label), "Slot %d", i);
        if (ImGui::Selectable(label, selectedSlot_ == i)) {
            selectedSlot_ = i;
        }

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Preview area
    ImGui::BeginChild("Preview", ImVec2(0, -footerHeight));
    ImGui::Text("Slot %d", selectedSlot_);
    ImGui::Separator();

    if (slotInfo_[selectedSlot_].occupied && slotInfo_[selectedSlot_].glTexture) {
        // Larger thumbnail
        ImGui::Image((ImTextureID)(intptr_t)slotInfo_[selectedSlot_].glTexture,
                     ImVec2(scaled(200), scaled(140)));
    } else if (slotInfo_[selectedSlot_].occupied) {
        ImGui::Text("(No preview available)");
    } else {
        ImGui::TextDisabled("Empty slot");
    }
    ImGui::EndChild();

    // Footer
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        Action a;
        a.type = ActionType::SaveState;
        a.intValue = selectedSlot_;
        pushAction(a);
    }
    ImGui::SameLine();
    bool slotEmpty = !slotInfo_[selectedSlot_].occupied;
    if (slotEmpty) ImGui::BeginDisabled();
    if (ImGui::Button("Load")) {
        Action a;
        a.type = ActionType::LoadState;
        a.intValue = selectedSlot_;
        pushAction(a);
        showSaveStates_ = false;
        visible_ = false;
    }
    if (slotEmpty) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && slotEmpty) {
        ImGui::SetTooltip("No save data in this slot");
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        showSaveStates_ = false;
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Cheat manager — complete redesign
// -----------------------------------------------------------------------

void AppUI::drawCheatManager() {
    prepareCenteredWindow(ImVec2(scaled(680), scaled(560)),
                          ImVec2(scaled(420), scaled(340)),
                          ImVec2(scaled(920), scaled(760)));
    if (!ImGui::Begin("Cheat Manager", &showCheatManager_)) {
        ImGui::End();
        return;
    }

    auto selectedRamResult = [&]() -> const RamSearchResultItem* {
        for (const auto& item : ramSearchResults_) {
            if (item.index == ramSearchSelectedIndex_) {
                return &item;
            }
        }
        return nullptr;
    };

    auto selectRamResult = [&](const RamSearchResultItem& item) {
        ramSearchSelectedIndex_ = item.index;
        std::snprintf(ramSearchTargetValueBuf_, sizeof(ramSearchTargetValueBuf_), "%s", item.currentDec.c_str());
        ramSearchTargetNameBuf_[0] = '\0';
    };

    if (ramSearchSelectedIndex_ >= 0 && !selectedRamResult()) {
        ramSearchSelectedIndex_ = -1;
        ramSearchTargetValueBuf_[0] = '\0';
        ramSearchTargetNameBuf_[0] = '\0';
    }

    if (ImGui::BeginTabBar("CheatTabs")) {
        if (ImGui::BeginTabItem("Cheats")) {
            ImGui::SeparatorText("Add New Cheat");

            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##code", "Raw code: FFFFFF:YY, FFFFFF:YYYY, or FFFFFF:XXXXXXXX",
                                     cheatCodeBuf_, sizeof(cheatCodeBuf_));

            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##name", "Description (optional)", cheatNameBuf_, sizeof(cheatNameBuf_));

            bool codeEmpty = (cheatCodeBuf_[0] == '\0');
            if (codeEmpty) ImGui::BeginDisabled();
            if (ImGui::Button("Add Cheat", ImVec2(-1, 0))) {
                Action a;
                a.type = ActionType::AddCheat;
                a.text = cheatCodeBuf_;
                a.text2 = cheatNameBuf_;
                pushAction(a);
                cheatCodeBuf_[0] = '\0';
                cheatNameBuf_[0] = '\0';
            }
            if (codeEmpty) ImGui::EndDisabled();

            ImGui::SeparatorText("Active Cheats");

            float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
            if (ImGui::BeginChild("CheatList", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders)) {
                if (cheats_.empty()) {
                    float availW = ImGui::GetContentRegionAvail().x;
                    float availH = ImGui::GetContentRegionAvail().y;
                    const char* mainText = "No cheats added yet";
                    const char* hintText = "Enter a raw address code above or create one from RAM Search";
                    ImVec2 mainSize = ImGui::CalcTextSize(mainText);
                    ImVec2 hintSize = ImGui::CalcTextSize(hintText);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (availH - mainSize.y - hintSize.y - 4) * 0.5f);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - mainSize.x) * 0.5f);
                    ImGui::TextDisabled("%s", mainText);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - hintSize.x) * 0.5f);
                    ImGui::TextDisabled("%s", hintText);
                }

                for (size_t i = 0; i < cheats_.size(); i++) {
                    auto& ch = cheats_[i];
                    ImGui::PushID(static_cast<int>(i));

                    bool enabled = ch.enabled;
                    if (ToggleSwitch("##toggle", &enabled)) {
                        Action a;
                        a.type = ActionType::ToggleCheat;
                        a.intValue = static_cast<int>(i);
                        pushAction(a);
                    }

                    ImGui::SameLine();
                    const float removeButtonWidth = ImGui::GetFrameHeight();
                    ImGui::BeginGroup();
                    if (ch.name.empty()) {
                        ImGui::TextDisabled("(unnamed)");
                    } else {
                        ImGui::TextUnformatted(ch.name.c_str());
                    }
                    ImGui::TextDisabled("%s", ch.code.c_str());
                    ImGui::EndGroup();

                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - removeButtonWidth);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.20f, 0.20f, 0.40f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.30f, 0.30f, 0.60f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.30f, 0.30f, 1.00f));
                    if (ImGui::Button("X", ImVec2(removeButtonWidth, 0))) {
                        Action a;
                        a.type = ActionType::RemoveCheat;
                        a.intValue = static_cast<int>(i);
                        pushAction(a);
                    }
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Remove cheat");
                    }

                    ImGui::PopID();

                    if (i + 1 < cheats_.size()) {
                        ImGui::Separator();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::Text("%d cheat(s)", static_cast<int>(cheats_.size()));

            if (!romLoaded_) ImGui::BeginDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Load File")) {
                Action a;
                a.type = ActionType::LoadCheatsFromFile;
                pushAction(a);
            }
            ImGui::SameLine();
            if (ImGui::Button("Save File")) {
                Action a;
                a.type = ActionType::SaveCheatsToFile;
                pushAction(a);
            }
            if (!romLoaded_) ImGui::EndDisabled();

            if (!cheats_.empty()) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - scaled(80));
                ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
                if (ImGui::Button("Clear All")) {
                    confirmClearCheats_ = true;
                    ImGui::OpenPopup("Confirm Clear");
                }
                ImGui::PopStyleColor(3);
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("RAM Search")) {
            auto searchTypeLabel = [](Cheats::CheatValueType type) -> const char* {
                return Cheats::valueTypeLabel(type);
            };
            auto heuristicLabel = [](Cheats::SearchHeuristic heuristic) -> const char* {
                switch (heuristic) {
                    case Cheats::SearchHeuristic::Off: return "Exact";
                    case Cheats::SearchHeuristic::BooleanLike: return "Boolean / tiny counter";
                    case Cheats::SearchHeuristic::Tolerance1: return "Tolerance +/-1";
                    case Cheats::SearchHeuristic::Tolerance2: return "Tolerance +/-2";
                }
                return "Exact";
            };
            auto emitSearchAction = [&](ActionType type, int intValue, int intValue2, const char* text, const char* text2 = "") {
                Action a;
                a.type = type;
                a.intValue = intValue;
                a.intValue2 = intValue2;
                a.text = text ? text : "";
                a.text2 = text2 ? text2 : "";
                pushAction(a);
            };

            if (!romLoaded_) {
                ImGui::TextDisabled("%s", "Load a ROM to search writable memory.");
                ImGui::Spacing();
            }

            if (!romLoaded_) ImGui::BeginDisabled();

            ImGui::SeparatorText("Search Setup");
            ImGui::TextDisabled("Mode");
            if (ImGui::RadioButton("Known Value", ramSearchKnownValueMode_)) {
                ramSearchKnownValueMode_ = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Unknown Initial State", !ramSearchKnownValueMode_)) {
                ramSearchKnownValueMode_ = false;
            }

            ImGui::TextDisabled("Value Type");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ram_type", searchTypeLabel(ramSearchType_))) {
                for (int i = 0; i <= static_cast<int>(Cheats::CheatValueType::S32); i++) {
                    const auto type = static_cast<Cheats::CheatValueType>(i);
                    const bool selected = (type == ramSearchType_);
                    if (ImGui::Selectable(searchTypeLabel(type), selected)) {
                        ramSearchType_ = type;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TextDisabled("Heuristic Match");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ram_heuristic", heuristicLabel(ramSearchHeuristic_))) {
                for (int i = 0; i <= static_cast<int>(Cheats::SearchHeuristic::Tolerance2); i++) {
                    const auto heuristic = static_cast<Cheats::SearchHeuristic>(i);
                    const bool selected = (heuristic == ramSearchHeuristic_);
                    if (ImGui::Selectable(heuristicLabel(heuristic), selected)) {
                        ramSearchHeuristic_ = heuristic;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TextDisabled("Value");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##ram_value", "Decimal or 0x-prefixed hex", ramSearchValueBuf_, sizeof(ramSearchValueBuf_));

            if (ramSearchKnownValueMode_) {
                if (ImGui::Button("Initial Search", ImVec2(-1, 0))) {
                    emitSearchAction(ActionType::StartRamSearchKnownValue,
                                     static_cast<int>(ramSearchType_),
                                     static_cast<int>(ramSearchHeuristic_),
                                     ramSearchValueBuf_);
                }
            } else {
                if (ImGui::Button("Initial Snapshot", ImVec2(-1, 0))) {
                    emitSearchAction(ActionType::StartRamSearchUnknown,
                                     static_cast<int>(ramSearchType_),
                                     0,
                                     "");
                }
            }

            ImGui::Spacing();
            bool canResetSearch = ramSearchActive_ || ramSearchResultCount_ > 0;
            if (!canResetSearch) ImGui::BeginDisabled();
            if (ImGui::Button("Reset Search", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::ResetRamSearch, 0, 0, "");
                ramSearchSelectedIndex_ = -1;
                ramSearchTargetValueBuf_[0] = '\0';
                ramSearchTargetNameBuf_[0] = '\0';
            }
            if (!canResetSearch) ImGui::EndDisabled();

            ImGui::SeparatorText("Refine");
            if (ramSearchActive_) {
                ImGui::TextDisabled("%d candidate(s)", ramSearchResultCount_);
            } else {
                ImGui::TextDisabled("%s", "Take an initial snapshot or known-value search first.");
            }

            if (!ramSearchActive_) ImGui::BeginDisabled();
            if (ImGui::Button("Changed", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::Changed),
                                 static_cast<int>(ramSearchHeuristic_),
                                 "");
            }
            if (ImGui::Button("Unchanged", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::Unchanged),
                                 static_cast<int>(ramSearchHeuristic_),
                                 "");
            }
            if (ImGui::Button("Increased", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::Increased),
                                 static_cast<int>(ramSearchHeuristic_),
                                 "");
            }
            if (ImGui::Button("Decreased", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::Decreased),
                                 static_cast<int>(ramSearchHeuristic_),
                                 "");
            }

            ImGui::TextDisabled("Value-Based Refine");
            if (ImGui::Button("Equal To Value", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::EqualToValue),
                                 static_cast<int>(ramSearchHeuristic_),
                                 ramSearchValueBuf_);
            }
            if (ImGui::Button("Not Equal To Value", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::RefineRamSearch,
                                 static_cast<int>(Cheats::SearchCompareMode::NotEqualToValue),
                                 static_cast<int>(ramSearchHeuristic_),
                                 ramSearchValueBuf_);
            }
            if (!ramSearchActive_) ImGui::EndDisabled();

            ImGui::SeparatorText("Results");
            ImGui::TextDisabled("%d result(s)%s",
                                ramSearchResultCount_,
                                ramSearchResultCount_ > static_cast<int>(ramSearchResults_.size()) ? ", showing first 500" : "");
            if (ImGui::BeginChild("RamSearchResults", ImVec2(0, scaled(180.0f)), ImGuiChildFlags_Borders)) {
                if (ramSearchResults_.empty()) {
                    ImGui::TextDisabled("%s", ramSearchActive_
                        ? "No matches remain. Reset or change the search criteria."
                        : "No search results yet.");
                } else if (ImGui::BeginTable("RamSearchTable", 5,
                                             ImGuiTableFlags_RowBg |
                                             ImGuiTableFlags_BordersInnerV |
                                             ImGuiTableFlags_ScrollY |
                                             ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Address");
                    ImGui::TableSetupColumn("Region");
                    ImGui::TableSetupColumn("Current");
                    ImGui::TableSetupColumn("Hex");
                    ImGui::TableSetupColumn("Previous");
                    ImGui::TableHeadersRow();

                    for (const auto& item : ramSearchResults_) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        char addressText[16];
                        std::snprintf(addressText, sizeof(addressText), "%06X", item.address & 0xFFFFFFu);
                        const bool selected = (item.index == ramSearchSelectedIndex_);
                        if (ImGui::Selectable(addressText, selected, ImGuiSelectableFlags_SpanAllColumns)) {
                            selectRamResult(item);
                        }
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(item.regionLabel.c_str());
                        ImGui::TableSetColumnIndex(2);
                        ImGui::TextUnformatted(item.currentDec.c_str());
                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(item.currentHex.c_str());
                        ImGui::TableSetColumnIndex(4);
                        ImGui::TextUnformatted(item.previousDec.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            ImGui::SeparatorText("Selected Result");
            const RamSearchResultItem* selected = selectedRamResult();
            if (selected) {
                char addressText[16];
                std::snprintf(addressText, sizeof(addressText), "%06X", selected->address & 0xFFFFFFu);
                ImGui::Text("Address: %s", addressText);
                ImGui::Text("Region: %s", selected->regionLabel.c_str());
                ImGui::Text("Type: %s", selected->typeLabel.c_str());
                ImGui::Text("Current: %s (%s)", selected->currentDec.c_str(), selected->currentHex.c_str());
                ImGui::Text("Previous: %s (%s)", selected->previousDec.c_str(), selected->previousHex.c_str());

                ImGui::TextDisabled("Target Value");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##ram_target_value", "Value to write or freeze",
                                         ramSearchTargetValueBuf_, sizeof(ramSearchTargetValueBuf_));

                ImGui::TextDisabled("Cheat Name");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##ram_target_name", "Optional description",
                                         ramSearchTargetNameBuf_, sizeof(ramSearchTargetNameBuf_));

                if (ImGui::Button("Set Once", ImVec2(-1, 0))) {
                    emitSearchAction(ActionType::SetRamSearchResultOnce,
                                     selected->index,
                                     0,
                                     ramSearchTargetValueBuf_);
                }
                if (ImGui::Button("Freeze As Cheat", ImVec2(-1, 0))) {
                    emitSearchAction(ActionType::FreezeRamSearchResult,
                                     selected->index,
                                     0,
                                     ramSearchTargetValueBuf_,
                                     ramSearchTargetNameBuf_);
                }
                if (ImGui::Button("Refresh Value", ImVec2(-1, 0))) {
                    emitSearchAction(ActionType::RefreshRamSearchResult,
                                     selected->index,
                                     0,
                                     "");
                }
            } else {
                ImGui::TextDisabled("%s", "Select a result row to inspect it, set a value once, or freeze it as a cheat.");
            }

            if (!romLoaded_) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    // Confirmation modal
    ImVec2 modalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Clear", &confirmClearCheats_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove all %d cheats? This cannot be undone.", (int)cheats_.size());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Cancel", ImVec2(scaled(100), 0))) {
            confirmClearCheats_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
        if (ImGui::Button("Clear All", ImVec2(scaled(100), 0))) {
            Action a;
            a.type = ActionType::ClearCheats;
            pushAction(a);
            confirmClearCheats_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Video settings
// -----------------------------------------------------------------------

void AppUI::drawVideoSettings() {
    prepareCenteredWindow(ImVec2(scaled(560), scaled(560)),
                          ImVec2(scaled(380), scaled(320)),
                          ImVec2(scaled(760), scaled(760)));
    if (!ImGui::Begin("Video Settings", &showVideoSettings_)) {
        ImGui::End();
        return;
    }

    int currentWindowWidth = 0;
    int currentWindowHeight = 0;
    if (window_) {
        SDL_GetWindowSize(window_, &currentWindowWidth, &currentWindowHeight);
    }

    auto emitBoolAction = [&](ActionType type, bool value) {
        Action a;
        a.type = type;
        a.boolValue = value;
        pushAction(a);
    };

    auto drawFieldLabel = [&](const char* text) {
        ImGui::TextDisabled("%s", text);
    };

    auto drawToggleRow = [&](const char* id,
                             const char* title,
                             const char* subtitle,
                             bool* value,
                             ActionType type) {
        ImGui::PushID(id);
        const float toggleWidth = ImGui::GetFrameHeight() * 1.8f;
        const float gap = scaled(12.0f);
        const float startX = ImGui::GetCursorPosX();
        const float startY = ImGui::GetCursorPosY();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float textWidth = std::max(0.0f, availWidth - toggleWidth - gap);
        const float wrapPos = ImGui::GetCursorScreenPos().x + textWidth;

        ImGui::PushTextWrapPos(wrapPos);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title);
        ImGui::PopTextWrapPos();

        const float rowBottomY = std::max(ImGui::GetCursorPosY(), startY + ImGui::GetFrameHeight());

        bool nextValue = *value;
        ImGui::SetCursorPosX(startX + std::max(0.0f, availWidth - toggleWidth));
        ImGui::SetCursorPosY(startY);
        if (ToggleSwitch("##toggle", &nextValue)) {
            *value = nextValue;
            emitBoolAction(type, nextValue);
        }

        ImGui::SetCursorPosX(startX);
        ImGui::SetCursorPosY(rowBottomY);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopID();
    };

    if (beginSettingsCard("##overview", "Display Surface",
                          fullscreenDisplayName_.empty() ? "Current display" : fullscreenDisplayName_.c_str())) {
        ImGui::TextDisabled("Presentation");
        ImGui::TextColored(UIColors::AccentTeal, "%s", fullscreenModeLabel(fullscreenMode_));
        ImGui::Spacing();
        ImGui::TextDisabled("Window");
        if (fullscreenMode_ == FullscreenMode::Windowed) {
            ImGui::Text("%d x %d", currentWindowWidth, currentWindowHeight);
        } else if (fullscreenMode_ == FullscreenMode::Exclusive &&
                   fullscreenResolutionIndex_ >= 0 &&
                   fullscreenResolutionIndex_ < static_cast<int>(fullscreenResolutions_.size())) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(fullscreenResolutions_[fullscreenResolutionIndex_].label.c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "Uses display desktop mode");
            ImGui::PopTextWrapPos();
        }
        endSettingsCard();
    }

    ImGui::Spacing();

    if (beginSettingsCard("##display", "Display Mode",
                          "Choose how the emulator occupies the screen.")) {
        const bool hasExclusiveModes = !fullscreenResolutions_.empty();
        drawFieldLabel("Presentation Mode");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##presentation_mode", fullscreenModeLabel(fullscreenMode_))) {
            for (int i = 0; i < 3; i++) {
                const FullscreenMode candidate = static_cast<FullscreenMode>(i);
                const bool disabled = (candidate == FullscreenMode::Exclusive && !hasExclusiveModes);
                if (disabled) ImGui::BeginDisabled();
                const bool selected = (candidate == fullscreenMode_);
                if (ImGui::Selectable(fullscreenModeLabel(candidate), selected)) {
                    fullscreenMode_ = candidate;
                    Action a;
                    a.type = ActionType::SetFullscreenMode;
                    a.intValue = i;
                    pushAction(a);
                }
                if (selected) ImGui::SetItemDefaultFocus();
                if (disabled) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }

        if (fullscreenMode_ == FullscreenMode::Windowed) {
            ImGui::Spacing();
            drawFieldLabel("Window Size");

            const int windowScales[] = {1, 2, 3, 4, 5, 6};
            char preview[64];
            std::snprintf(preview, sizeof(preview), "%dx   %d x %d",
                          windowScale_, 320 * windowScale_, 224 * windowScale_);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##window_size", preview)) {
                for (int scale : windowScales) {
                    char label[64];
                    std::snprintf(label, sizeof(label), "%dx   %d x %d",
                                  scale, 320 * scale, 224 * scale);
                    const bool selected = (scale == windowScale_);
                    if (ImGui::Selectable(label, selected)) {
                        windowScale_ = scale;
                        Action a;
                        a.type = ActionType::SetWindowScale;
                        a.intValue = scale;
                        pushAction(a);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "Windowed mode uses exact Genesis multiples so the scaling stays predictable.");
            ImGui::PopTextWrapPos();
        } else if (fullscreenMode_ == FullscreenMode::BorderlessDesktop) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", "Borderless fullscreen always uses the desktop resolution of the current display.");
            ImGui::TextDisabled("%s", "Switch to Exclusive Fullscreen if you want to choose a specific display mode.");
            ImGui::PopTextWrapPos();
        } else {
            ImGui::Spacing();
            if (!hasExclusiveModes) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", "No exclusive fullscreen modes were reported by SDL for this display.");
                ImGui::PopTextWrapPos();
            } else {
                const char* selectedLabel = (fullscreenResolutionIndex_ >= 0 &&
                                             fullscreenResolutionIndex_ < static_cast<int>(fullscreenResolutions_.size()))
                                                ? fullscreenResolutions_[fullscreenResolutionIndex_].label.c_str()
                                                : "Select a mode";
                drawFieldLabel("Fullscreen Resolution");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##fullscreen_resolution", selectedLabel)) {
                    for (int i = 0; i < static_cast<int>(fullscreenResolutions_.size()); i++) {
                        const bool selected = (i == fullscreenResolutionIndex_);
                        if (ImGui::Selectable(fullscreenResolutions_[i].label.c_str(), selected)) {
                            fullscreenResolutionIndex_ = i;
                            Action a;
                            a.type = ActionType::SetFullscreenResolution;
                            a.intValue = i;
                            pushAction(a);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextDisabled("%s", "Exclusive fullscreen switches the display mode before presenting frames.");
                ImGui::PopTextWrapPos();
            }
        }
        endSettingsCard();
    }

    ImGui::Spacing();

    if (beginSettingsCard("##scaling", "Image Scaling",
                          "Control how the Genesis frame is fitted into the available space.")) {
        static const char* scalingLabels[] = {"Integer Only", "Fit", "Stretch"};
        static const char* scalingDescriptions[] = {
            "Crisp pixel multiples with no fractional scaling.",
            "Fill as much space as possible while preserving the chosen aspect ratio.",
            "Stretch to the full viewport, regardless of aspect."
        };
        int scalingIdx = static_cast<int>(scalingMode_);
        drawFieldLabel("Scaling Mode");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##scaling_mode", scalingLabels[scalingIdx])) {
            for (int i = 0; i < 3; i++) {
                const bool selected = (i == scalingIdx);
                if (ImGui::Selectable(scalingLabels[i], selected)) {
                    scalingMode_ = static_cast<ScalingMode>(i);
                    Action a;
                    a.type = ActionType::SetScalingMode;
                    a.intValue = i;
                    pushAction(a);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", scalingDescriptions[scalingIdx]);
        ImGui::PopTextWrapPos();

        static const char* aspectLabels[] = {"Auto", "4:3", "16:9", "Stretch"};
        static const char* aspectDescriptions[] = {
            "Use the framebuffer's native display aspect.",
            "Classic TV aspect ratio.",
            "Wide presentation for modern displays.",
            "Fill the viewport width and height."
        };
        int aspectIdx = static_cast<int>(aspectRatio_);
        drawFieldLabel("Aspect Ratio");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##aspect_ratio", aspectLabels[aspectIdx])) {
            for (int i = 0; i < 4; i++) {
                const bool selected = (i == aspectIdx);
                if (ImGui::Selectable(aspectLabels[i], selected)) {
                    aspectRatio_ = static_cast<AspectRatio>(i);
                    Action a;
                    a.type = ActionType::SetAspectRatio;
                    a.intValue = i;
                    pushAction(a);
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", aspectDescriptions[aspectIdx]);
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        drawToggleRow("bilinear", "Bilinear Filtering",
                      "Smooth pixel edges when the frame is scaled beyond integer multiples.",
                      &bilinearFiltering_, ActionType::SetBilinearFiltering);
        endSettingsCard();
    }

    ImGui::Spacing();

    if (beginSettingsCard("##presentation", "Presentation & Timing",
                          "Tune frame pacing and runtime overlays.")) {
        drawToggleRow("vsync", "VSync",
                      "Match frame presentation to the monitor refresh rate when supported.",
                      &vsyncEnabled_, ActionType::SetVSync);
        ImGui::Spacing();
        drawToggleRow("limiter", "Frame Limiter",
                      "Keep emulation speed near the console's original refresh rate.",
                      &frameLimiterEnabled_, ActionType::SetFrameLimiter);
        ImGui::Spacing();
        drawToggleRow("fps", "FPS Counter",
                      "Show a small framerate overlay while the game is running.",
                      &showFPS_, ActionType::ToggleFPS);
        if (showFPS_) {
            ImGui::Spacing();
            ImGui::TextDisabled("Profiler Mode");
            const float buttonWidth = scaled(110);
            auto drawModeButton = [&](const char* label, ProfilerMode modeValue) {
                const bool selected = (profilerMode_ == modeValue);
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.47f, 0.62f, 0.92f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.29f, 0.56f, 0.72f, 0.96f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.39f, 0.52f, 0.98f));
                }
                if (ImGui::Button(label, ImVec2(buttonWidth, 0))) {
                    profilerMode_ = modeValue;
                    Action a;
                    a.type = ActionType::SetProfilerMode;
                    a.intValue = static_cast<int>(modeValue);
                    pushAction(a);
                }
                if (selected) {
                    ImGui::PopStyleColor(3);
                }
            };
            drawModeButton("Simple", ProfilerMode::Simple);
            drawModeButton("Detailed", ProfilerMode::Detailed);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s",
                                profilerMode_ == ProfilerMode::Detailed
                                    ? "Detailed mode adds emulator-side timing overhead."
                                    : "Simple mode keeps the overlay lightweight.");
            ImGui::PopTextWrapPos();
        }
        endSettingsCard();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Audio settings
// -----------------------------------------------------------------------

void AppUI::drawAudioSettings() {
    prepareCenteredWindow(ImVec2(scaled(380), scaled(240)),
                          ImVec2(scaled(300), scaled(180)),
                          ImVec2(scaled(500), scaled(300)));
    if (!ImGui::Begin("Audio Settings", &showAudioSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Buffer");

    ImGui::Text("Audio Buffer Size");

    // Quick presets as radio buttons
    if (ImGui::RadioButton("Low (75ms)", audioQueueMs_ == 75)) {
        audioQueueMs_ = 75;
        Action a; a.type = ActionType::SetAudioQueueMs; a.intValue = audioQueueMs_; pushAction(a);
    }
    if (ImGui::RadioButton("Med (150ms)", audioQueueMs_ == 150)) {
        audioQueueMs_ = 150;
        Action a; a.type = ActionType::SetAudioQueueMs; a.intValue = audioQueueMs_; pushAction(a);
    }
    if (ImGui::RadioButton("High (300ms)", audioQueueMs_ == 300)) {
        audioQueueMs_ = 300;
        Action a; a.type = ActionType::SetAudioQueueMs; a.intValue = audioQueueMs_; pushAction(a);
    }

    // Fine-tune slider
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##audiobuf", &audioQueueMs_, 50, 500, "%d ms")) {
        Action a;
        a.type = ActionType::SetAudioQueueMs;
        a.intValue = audioQueueMs_;
        pushAction(a);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lower values reduce latency but may crackle.\nHigher values are more stable but add delay.");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Recommended: 100-250ms");
    ImGui::PopTextWrapPos();

    ImGui::End();
}

// -----------------------------------------------------------------------
// Emulation settings
// -----------------------------------------------------------------------

void AppUI::drawEmuSettings() {
    prepareCenteredWindow(ImVec2(scaled(420), scaled(220)),
                          ImVec2(scaled(320), scaled(170)),
                          ImVec2(scaled(560), scaled(340)));
    if (!ImGui::Begin("Emulation Settings", &showEmuSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Options");
    static const char* modeLabels[] = {"Auto", "NTSC", "PAL"};
    int modeIndex = static_cast<int>(videoStandardMode_);
    ImGui::TextDisabled("%s", "Video Standard");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##video_standard", modeLabels[modeIndex])) {
        for (int i = 0; i < 3; i++) {
            const bool selected = (i == modeIndex);
            if (ImGui::Selectable(modeLabels[i], selected)) {
                videoStandardMode_ = static_cast<VideoStandardMode>(i);
                Action a;
                a.type = ActionType::SetVideoStandard;
                a.intValue = i;
                pushAction(a);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Auto follows the ROM header when possible and falls back to NTSC.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    bool autoLoad = autoLoadState_;
    if (ImGui::Checkbox("Auto-load latest save state", &autoLoad)) {
        autoLoadState_ = autoLoad;
        Action a;
        a.type = ActionType::SetAutoLoadState;
        a.boolValue = autoLoad;
        pushAction(a);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Automatically loads the most recent save state when opening a ROM.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const char* activeLabel = effectiveVideoStandard_ == VideoStandard::PAL ? "PAL" : "NTSC";
    ImGui::Text("Current standard: %s", activeLabel);
    ImGui::Text("Refresh rate: %.3f Hz", effectiveRefreshHz_);
    ImGui::Text("Scanlines/frame: %d", effectiveScanlines_);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Genesis NTSC runs at about 59.923 Hz, not 59.97 Hz.");
    ImGui::PopTextWrapPos();

    ImGui::End();
}

// -----------------------------------------------------------------------
// Input settings (key rebinding)
// -----------------------------------------------------------------------

void AppUI::drawBindingButton(BindableAction action, const ImVec2& size) {
    if (!keyBindings_) {
        return;
    }

    const int actionIndex = static_cast<int>(action);
    const char* keyStr = KeyBindings::keyName(keyBindings_->get(action));
    ImVec2 buttonSize = size;
    if (buttonSize.x <= 0.0f) {
        buttonSize.x = ImGui::GetContentRegionAvail().x;
    }

    ImGui::PushID(actionIndex);
    if (listeningAction_ == actionIndex) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.22f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.34f, 0.26f, 1.0f));
        ImGui::Button("Press a key...", buttonSize);
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button((keyStr && keyStr[0]) ? keyStr : "(none)", buttonSize)) {
            listeningAction_ = actionIndex;
        }
    }
    ImGui::PopID();
}

void AppUI::drawBindingGroup(int startIdx, int endIdx) {
    if (ImGui::BeginTable("##bindings", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(150));

        for (int i = startIdx; i < endIdx && i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            const char* name = KeyBindings::actionName(action);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(name);
            ImGui::PopTextWrapPos();

            ImGui::TableNextColumn();
            drawBindingButton(action, ImVec2(0.0f, 0.0f));
        }

        ImGui::EndTable();
    }
}

void AppUI::drawControllerBindingDiagram() {
    struct Callout {
        BindableAction action;
        const char* label;
        ImVec2 anchorNorm;
        ImVec2 fieldNorm;
    };

    const float sectionHeight = scaled(420.0f);
    const float fieldWidth = scaled(116.0f);
    const float fieldHeight = ImGui::GetFrameHeight();
    const float labelGap = scaled(4.0f);

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Click a field, then press a keyboard key to bind that controller button.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (!ImGui::BeginChild("ControllerDiagram", ImVec2(0, sectionHeight),
                           ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.y < scaled(360.0f)) {
        canvasSize.y = scaled(360.0f);
    }

    ImGui::Dummy(canvasSize);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImRect canvas(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y));

    const ImU32 panelFill = IM_COL32(14, 20, 40, 245);
    const ImU32 panelEdge = IM_COL32(46, 64, 110, 255);
    const ImU32 shellFill = IM_COL32(44, 54, 84, 255);
    const ImU32 shellEdge = IM_COL32(108, 128, 184, 255);
    const ImU32 padFill = IM_COL32(20, 24, 38, 255);
    const ImU32 accentLine = IM_COL32(110, 190, 220, 255);
    const ImU32 actionFill = IM_COL32(223, 185, 78, 255);
    const ImU32 actionFillAlt = IM_COL32(206, 112, 76, 255);
    const ImU32 startFill = IM_COL32(90, 160, 115, 255);

    draw->AddRectFilled(canvas.Min, canvas.Max, panelFill, scaled(18.0f));
    draw->AddRect(canvas.Min, canvas.Max, panelEdge, scaled(18.0f), 0, scaled(1.5f));

    auto pt = [&](float x, float y) {
        return ImVec2(canvas.Min.x + canvasSize.x * x, canvas.Min.y + canvasSize.y * y);
    };

    const ImVec2 shellMin = pt(0.16f, 0.27f);
    const ImVec2 shellMax = pt(0.84f, 0.77f);
    const float gripRadius = scaled(58.0f);
    draw->AddCircleFilled(pt(0.22f, 0.61f), gripRadius, shellFill, 32);
    draw->AddCircleFilled(pt(0.78f, 0.61f), gripRadius, shellFill, 32);
    draw->AddRectFilled(shellMin, shellMax, shellFill, scaled(32.0f));
    draw->AddRect(shellMin, shellMax, shellEdge, scaled(32.0f), 0, scaled(1.5f));
    draw->AddCircle(pt(0.22f, 0.61f), gripRadius, shellEdge, 32, scaled(1.5f));
    draw->AddCircle(pt(0.78f, 0.61f), gripRadius, shellEdge, 32, scaled(1.5f));

    draw->AddRectFilled(pt(0.26f, 0.43f), pt(0.35f, 0.47f), padFill, scaled(8.0f));
    draw->AddRectFilled(pt(0.285f, 0.39f), pt(0.325f, 0.51f), padFill, scaled(8.0f));

    const ImVec2 startCenter = pt(0.50f, 0.56f);
    const ImVec2 modeCenter = pt(0.50f, 0.47f);
    draw->AddRectFilled(ImVec2(startCenter.x - scaled(22.0f), startCenter.y - scaled(10.0f)),
                        ImVec2(startCenter.x + scaled(22.0f), startCenter.y + scaled(10.0f)),
                        startFill, scaled(10.0f));
    draw->AddRectFilled(ImVec2(modeCenter.x - scaled(18.0f), modeCenter.y - scaled(8.0f)),
                        ImVec2(modeCenter.x + scaled(18.0f), modeCenter.y + scaled(8.0f)),
                        IM_COL32(88, 96, 130, 255), scaled(8.0f));

    const ImVec2 xCenter = pt(0.65f, 0.41f);
    const ImVec2 yCenter = pt(0.73f, 0.38f);
    const ImVec2 zCenter = pt(0.81f, 0.41f);
    const ImVec2 aCenter = pt(0.66f, 0.55f);
    const ImVec2 bCenter = pt(0.74f, 0.52f);
    const ImVec2 cCenter = pt(0.82f, 0.55f);
    const float buttonRadius = scaled(14.0f);
    draw->AddCircleFilled(xCenter, buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(yCenter, buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(zCenter, buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(aCenter, buttonRadius, actionFill, 24);
    draw->AddCircleFilled(bCenter, buttonRadius, actionFill, 24);
    draw->AddCircleFilled(cCenter, buttonRadius, actionFill, 24);

    static const Callout callouts[] = {
        {BindableAction::PadUp, "Up",       ImVec2(0.305f, 0.39f), ImVec2(0.03f, 0.06f)},
        {BindableAction::PadLeft, "Left",   ImVec2(0.26f, 0.45f),  ImVec2(0.03f, 0.28f)},
        {BindableAction::PadDown, "Down",   ImVec2(0.305f, 0.51f), ImVec2(0.03f, 0.50f)},
        {BindableAction::PadRight, "Right", ImVec2(0.35f, 0.45f),  ImVec2(0.03f, 0.72f)},
        {BindableAction::ButtonMode, "Mode",  ImVec2(0.50f, 0.47f), ImVec2(0.36f, 0.80f)},
        {BindableAction::ButtonStart, "Start", ImVec2(0.50f, 0.56f), ImVec2(0.50f, 0.80f)},
        {BindableAction::ButtonX, "X", ImVec2(0.65f, 0.41f), ImVec2(0.82f, 0.08f)},
        {BindableAction::ButtonY, "Y", ImVec2(0.73f, 0.38f), ImVec2(0.82f, 0.24f)},
        {BindableAction::ButtonZ, "Z", ImVec2(0.81f, 0.41f), ImVec2(0.82f, 0.40f)},
        {BindableAction::ButtonA, "A", ImVec2(0.66f, 0.55f), ImVec2(0.82f, 0.56f)},
        {BindableAction::ButtonB, "B", ImVec2(0.74f, 0.52f), ImVec2(0.82f, 0.72f)},
        {BindableAction::ButtonC, "C", ImVec2(0.82f, 0.55f), ImVec2(0.82f, 0.88f)},
    };

    const float labelHeight = ImGui::GetTextLineHeight();
    for (const Callout& callout : callouts) {
        ImVec2 labelPos(canvas.Min.x + canvasSize.x * callout.fieldNorm.x,
                        canvas.Min.y + canvasSize.y * callout.fieldNorm.y);
        if (callout.fieldNorm.x > 0.75f) {
            labelPos.x = canvas.Max.x - fieldWidth - scaled(12.0f);
        } else if (callout.fieldNorm.x > 0.34f && callout.fieldNorm.x < 0.7f) {
            labelPos.x -= fieldWidth * 0.5f;
        }
        if (labelPos.x < canvas.Min.x + scaled(12.0f)) {
            labelPos.x = canvas.Min.x + scaled(12.0f);
        }

        ImVec2 fieldPos(labelPos.x, labelPos.y + labelHeight + labelGap);
        const ImVec2 anchor = pt(callout.anchorNorm.x, callout.anchorNorm.y);
        const ImRect fieldRect(fieldPos, ImVec2(fieldPos.x + fieldWidth, fieldPos.y + fieldHeight));

        ImVec2 target;
        if (fieldRect.Max.x < anchor.x) {
            target = ImVec2(fieldRect.Max.x, fieldRect.GetCenter().y);
        } else if (fieldRect.Min.x > anchor.x) {
            target = ImVec2(fieldRect.Min.x, fieldRect.GetCenter().y);
        } else {
            target = ImVec2(fieldRect.GetCenter().x, fieldRect.Min.y);
        }

        const float midX = (anchor.x + target.x) * 0.5f;
        draw->AddCircleFilled(anchor, scaled(3.0f), accentLine, 12);
        draw->AddLine(anchor, ImVec2(midX, anchor.y), accentLine, scaled(1.5f));
        draw->AddLine(ImVec2(midX, anchor.y), ImVec2(midX, target.y), accentLine, scaled(1.5f));
        draw->AddLine(ImVec2(midX, target.y), target, accentLine, scaled(1.5f));

        ImGui::SetCursorScreenPos(labelPos);
        ImGui::TextUnformatted(callout.label);
        ImGui::SetCursorScreenPos(fieldPos);
        drawBindingButton(callout.action, ImVec2(fieldWidth, fieldHeight));
    }

    ImGui::SetCursorScreenPos(ImVec2(canvas.Min.x, canvas.Max.y));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();
}

void AppUI::drawInputSettings() {
    prepareCenteredWindow(ImVec2(scaled(760), scaled(760)),
                          ImVec2(scaled(560), scaled(520)),
                          ImVec2(scaled(980), scaled(940)));
    if (!ImGui::Begin("Input Settings", &showInputSettings_)) {
        ImGui::End();
        listeningAction_ = -1; // cancel listening if window closed
        return;
    }

    if (!keyBindings_) {
        ImGui::Text("No keybindings available");
        ImGui::End();
        return;
    }

    float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("BindingScroll", ImVec2(0, -footerHeight), ImGuiChildFlags_None)) {
        ImGui::SeparatorText("Controller");
        drawControllerBindingDiagram();

        ImGui::Spacing();

        ImGui::SeparatorText("Hotkeys");
        drawBindingGroup(12, KeyBindings::actionCount());
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Reset Defaults")) {
        confirmResetBindings_ = true;
        ImGui::OpenPopup("Confirm Reset");
    }

    // Confirmation modal
    ImVec2 modalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Reset", &confirmResetBindings_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Reset all key bindings to defaults?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button("Cancel", ImVec2(scaled(100), 0))) {
            confirmResetBindings_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
        if (ImGui::Button("Reset", ImVec2(scaled(100), 0))) {
            keyBindings_->loadDefaults();
            keyBindings_->save();
            Action a;
            a.type = ActionType::SetKeyBinding;
            pushAction(a);
            confirmResetBindings_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::EndPopup();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// About
// -----------------------------------------------------------------------

void AppUI::drawAbout() {
    prepareCenteredWindow(ImVec2(scaled(320), scaled(200)),
                          ImVec2(scaled(260), scaled(170)),
                          ImVec2(scaled(440), scaled(300)));
    if (!ImGui::Begin("About", &showAbout_)) {
        ImGui::End();
        return;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(NULL, style.FontSizeBase * 1.5f);
    ImGui::Text("Genesis Emulator");
    ImGui::PopFont();

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("A Sega Genesis / Mega Drive emulator");
    ImGui::Spacing();
    ImGui::TextUnformatted("CPU: Motorola 68000 + Zilog Z80");
    ImGui::TextUnformatted("Video: Yamaha YM7101 VDP");
    ImGui::TextUnformatted("Audio: Yamaha YM2612 + TI SN76489 PSG");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Text("Coded by pagefault");

    ImGui::End();
}

// -----------------------------------------------------------------------
// Controls
// -----------------------------------------------------------------------

void AppUI::drawControls() {
    prepareCenteredWindow(ImVec2(scaled(380), scaled(400)),
                          ImVec2(scaled(300), scaled(280)),
                          ImVec2(scaled(540), scaled(620)));
    if (!ImGui::Begin("Controls", &showControls_)) {
        ImGui::End();
        return;
    }

    if (!keyBindings_) {
        ImGui::Text("No keybindings loaded");
        ImGui::End();
        return;
    }

    // Gamepad section
    ImGui::SeparatorText("Gamepad");
    if (ImGui::BeginTable("controls_pad", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(140));

        for (int i = 0; i < 12 && i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(KeyBindings::actionName(action));
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::Text("%s", KeyBindings::keyName(keyBindings_->get(action)));
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();

    // Hotkeys section
    ImGui::SeparatorText("Hotkeys");
    if (ImGui::BeginTable("controls_hotkeys", 2,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(140));

        for (int i = 12; i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(KeyBindings::actionName(action));
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::Text("%s", KeyBindings::keyName(keyBindings_->get(action)));
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// -----------------------------------------------------------------------
// Key rebinding (called from main loop on SDL_KEYDOWN)
// -----------------------------------------------------------------------

void AppUI::feedKeyForRebind(SDL_Keycode key) {
    if (listeningAction_ < 0 || !keyBindings_) return;
    keyBindings_->set(static_cast<BindableAction>(listeningAction_), key);
    keyBindings_->save();
    listeningAction_ = -1;
}
