// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/app_ui.h"
#include "config_dir.h"

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
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(str_id);
    const float height = ImGui::GetFrameHeight();
    const float width = height * 1.8f;
    const float radius = height * 0.5f;
    ImVec2 pos = window->DC.CursorPos;
    ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
    ImGui::ItemSize(bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(bb, id)) return false;
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) { *v = !*v; ImGui::MarkItemEdited(id); }
    float t = *v ? 1.0f : 0.0f;
    if (g.LastActiveId == id) {
        float t_anim = ImSaturate(g.LastActiveIdTimer / 0.10f);
        t = *v ? t_anim : 1.0f - t_anim;
    }
    ImU32 col_bg;
    if (*v) col_bg = ImGui::GetColorU32(hovered ? ImVec4(0.25f, 0.72f, 0.35f, 1.0f) : ImVec4(0.20f, 0.65f, 0.30f, 1.0f));
    else    col_bg = ImGui::GetColorU32(hovered ? ImVec4(0.40f, 0.40f, 0.50f, 1.0f) : ImVec4(0.30f, 0.30f, 0.40f, 1.0f));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);
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
        ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
        return false;
    }
    ImGui::TextUnformatted(title);
    if (subtitle && subtitle[0] != '\0') {
        ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s", subtitle); ImGui::PopTextWrapPos();
    }
    ImGui::Spacing();
    return true;
}

static void endSettingsCard() {
    ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}

// -----------------------------------------------------------------------
// Init / Shutdown
// -----------------------------------------------------------------------

void AppUI::init(SDL_Window* window, void* backendContext) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    window_ = window;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.Fonts->AddFontDefaultVector();
    updateScaleMetrics(true);
#ifdef USE_VULKAN
    ImGui_ImplSDL3_InitForVulkan(window);
    (void)backendContext;
#else
    ImGui_ImplSDL3_InitForOpenGL(window, backendContext);
    ImGui_ImplOpenGL3_Init("#version 150");
#endif
    loadRecentRoms();
}

void AppUI::shutdown() {
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
    return false;
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
        overlayAnimT_ = std::min(overlayAnimT_ + ImGui::GetIO().DeltaTime / 0.15f, 1.0f);
        drawOverlay();
        // Gamepad B: go back to Resume, or close if already on Resume
        if (!ImGui::IsPopupOpen((const char*)nullptr, ImGuiPopupFlags_AnyPopup)
            && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
            if (activePanel_ != Panel::Resume) {
                activePanel_ = Panel::Resume;
            } else {
                visible_ = false;
                Action a; a.type = ActionType::CloseMenu; pushAction(a);
            }
        }
    } else {
        overlayAnimT_ = 0.0f;
    }
    return pending_;
}

void AppUI::render() {
    ImGui::Render();
#ifndef USE_VULKAN
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
}

// -----------------------------------------------------------------------
// State helpers
// -----------------------------------------------------------------------

bool AppUI::wantCaptureKeyboard() const { return ImGui::GetIO().WantCaptureKeyboard; }
bool AppUI::wantCaptureMouse() const { return ImGui::GetIO().WantCaptureMouse; }

void AppUI::setCurrentRomPath(const std::string& p) {
    std::filesystem::path romPath(p);
    if (romPath.has_parent_path()) currentDir_ = romPath.parent_path();
    currentRomName_ = romPath.stem().string();
}

void AppUI::setStatus(const std::string& text) { statusText_ = text; statusTimer_ = 3.0f; }

void AppUI::setFullscreenResolutions(const std::vector<FullscreenResolution>& modes,
                                     int selectedIndex, const std::string& displayName) {
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
    int windowWidth = 0, windowHeight = 0;
    SDL_GetWindowSize(window_, &windowWidth, &windowHeight);
    int drawableWidth = 0, drawableHeight = 0;
    SDL_GetWindowSizeInPixels(window_, &drawableWidth, &drawableHeight);
    if (!force && windowWidth == lastWindowWidth_ && windowHeight == lastWindowHeight_ &&
        drawableWidth == lastDrawableWidth_ && drawableHeight == lastDrawableHeight_) return;
    lastWindowWidth_ = windowWidth; lastWindowHeight_ = windowHeight;
    lastDrawableWidth_ = drawableWidth; lastDrawableHeight_ = drawableHeight;
    const float contentScale = SDL_GetWindowDisplayScale(window_);
    const float framebufferScale = (windowWidth > 0) ? static_cast<float>(drawableWidth) / static_cast<float>(windowWidth) : 1.0f;
    physicalDpiScale_ = std::clamp(std::max(contentScale, framebufferScale), 1.0f, 3.0f);
    const float scaleX = (windowWidth > 0) ? static_cast<float>(windowWidth) / UI_REFERENCE_WIDTH : 1.0f;
    const float scaleY = (windowHeight > 0) ? static_cast<float>(windowHeight) / UI_REFERENCE_HEIGHT : 1.0f;
    const float uiMagnification = 2.0f;
    layoutScale_ = std::clamp(std::min(scaleX, scaleY), 0.50f, 1.00f);
    uiScale_ = layoutScale_ * uiMagnification;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    applyStyle();
    style.FontSizeBase = 13.0f * uiMagnification;
    style.FontScaleDpi = 1.0f;
    style.FontScaleMain = layoutScale_;
    style.ScaleAllSizes(layoutScale_ * uiMagnification);
}

void AppUI::prepareCenteredWindow(const ImVec2& desiredSize, const ImVec2& minSize, const ImVec2& maxSize) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float marginX = scaled(28.0f), marginY = scaled(44.0f);
    const ImVec2 available(std::max(1.0f, viewport->WorkSize.x - marginX), std::max(1.0f, viewport->WorkSize.y - marginY));
    const ImVec2 constrainedMax(std::min(maxSize.x, available.x), std::min(maxSize.y, available.y));
    const ImVec2 constrainedMin(std::min(minSize.x, constrainedMax.x), std::min(minSize.y, constrainedMax.y));
    const ImVec2 constrainedDesired(std::clamp(desiredSize.x, constrainedMin.x, constrainedMax.x),
                                    std::clamp(desiredSize.y, constrainedMin.y, constrainedMax.y));
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(constrainedDesired, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(constrainedMin, constrainedMax);
}

void AppUI::pushAction(Action a) { if (pending_.type == ActionType::None) pending_ = a; }

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
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 320, 224, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, thumbnail);
        } else {
            glBindTexture(GL_TEXTURE_2D, slotInfo_[slot].glTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 320, 224, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, thumbnail);
        }
    }
#else
    (void)thumbnail;
#endif
}

// -----------------------------------------------------------------------
// Recent ROMs
// -----------------------------------------------------------------------

std::filesystem::path AppUI::getConfigDir() { return ::getConfigDir(); }

void AppUI::addRecentRom(const std::string& path) {
    std::string canonical = path;
    std::error_code ec;
    auto resolved = std::filesystem::canonical(path, ec);
    if (!ec) canonical = resolved.string();
    recentRoms_.erase(std::remove_if(recentRoms_.begin(), recentRoms_.end(),
                      [&](const RecentRom& r) { return r.path == canonical; }), recentRoms_.end());
    RecentRom r; r.path = canonical; r.name = std::filesystem::path(canonical).filename().string();
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
        std::error_code ec;
        auto resolved = std::filesystem::canonical(line, ec);
        if (!ec) line = resolved.string();
        bool dup = false;
        for (const auto& r : recentRoms_) { if (r.path == line) { dup = true; break; } }
        if (dup) continue;
        RecentRom r; r.path = line; r.name = std::filesystem::path(line).filename().string();
        recentRoms_.push_back(r);
    }
}

void AppUI::saveRecentRoms() {
    auto path = getConfigDir() / "recent_roms.txt";
    std::ofstream out(path);
    for (const auto& r : recentRoms_) out << r.path << "\n";
}

// -----------------------------------------------------------------------
// Style
// -----------------------------------------------------------------------

void AppUI::applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 14.0f; s.FrameRounding = 10.0f; s.GrabRounding = 10.0f;
    s.ChildRounding = 12.0f; s.PopupRounding = 12.0f; s.TabRounding = 10.0f;
    s.ScrollbarRounding = 10.0f;
    s.WindowBorderSize = 1.0f; s.ChildBorderSize = 1.0f; s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f; s.TabBorderSize = 0.0f;
    s.WindowPadding = ImVec2(14.0f, 14.0f); s.FramePadding = ImVec2(10.0f, 6.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f); s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.CellPadding = ImVec2(8.0f, 6.0f); s.ScrollbarSize = 14.0f; s.IndentSpacing = 18.0f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_Right;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 2.0f; s.SeparatorTextPadding = ImVec2(16.0f, 6.0f);
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(scaled(8), scaled(8)));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, scaled(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(scaled(10), scaled(8)));
    if (ImGui::Begin("##fps", nullptr, flags)) {
        ImVec4 fpsColor;
        if (stats.fps >= 55) fpsColor = UIColors::Success;
        else if (stats.fps >= 45) fpsColor = UIColors::Warning;
        else fpsColor = UIColors::Error;
        const float busyMs = std::max(stats.busyMs, 0.0001f);
        const float frameMs = std::max(stats.frameMs, 0.0001f);
        const float busyPctOfFrame = std::clamp((stats.busyMs / frameMs) * 100.0f, 0.0f, 100.0f);
        ImGui::TextColored(fpsColor, "FPS %d", stats.fps);
        ImGui::SameLine(); ImGui::TextDisabled("| %.2f ms frame", stats.frameMs);
        ImGui::Text("Busy %.2f ms (%.0f%% of frame)", stats.busyMs, busyPctOfFrame);
        enum class PercentBase { Busy, Emulation, Frame };
        struct Row { const char* label; float ms; ImVec4 color; PercentBase base; };
        const float emuMs = std::max(stats.emulationMs, 0.0001f);
        const Row simpleRows[] = {
            {"Emu",       stats.emulationMs, UIColors::AccentAmber, PercentBase::Busy},
            {"Render",    stats.videoMs,     UIColors::AccentTeal,  PercentBase::Busy},
            {"Swap",      stats.presentMs,   UIColors::Info,        PercentBase::Busy},
            {"UI",        stats.uiMs,        UIColors::Accent,      PercentBase::Busy},
            {"SDL",       stats.sdlMs,       UIColors::Success,     PercentBase::Busy},
            {"Audio I/O", stats.audioMs,     UIColors::Warning,     PercentBase::Busy},
            {"Other",     stats.otherMs,     ImVec4(0.68f, 0.64f, 0.84f, 1.00f), PercentBase::Busy},
            {"Idle",      stats.pacingMs,    ImVec4(0.42f, 0.48f, 0.58f, 1.00f), PercentBase::Frame},
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
                    case PercentBase::Busy: denominator = busyMs; break;
                    case PercentBase::Emulation: denominator = emuMs; suffix = " emu"; break;
                    case PercentBase::Frame: denominator = frameMs; suffix = " frame"; break;
                }
                const float pct = std::clamp((row.ms / denominator) * 100.0f, 0.0f, 999.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextColored(row.color, "%s", row.label);
                ImGui::TableNextColumn(); ImGui::Text("%.2f ms", row.ms);
                ImGui::TableNextColumn(); ImGui::Text("%.0f%%%s", pct, suffix);
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
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
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
// Overlay framework
// -----------------------------------------------------------------------

void AppUI::drawOverlay() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Dim background with blue tint
    ImDrawList* bgDraw = ImGui::GetBackgroundDrawList();
    float alpha = 0.88f * overlayAnimT_;
    bgDraw->AddRectFilled(viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(2, 4, 14, static_cast<int>(alpha * 255)));

    // Full-screen overlay window
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoBackground;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("##overlay", nullptr, flags)) {
        float sidebarWidth = scaled(190);
        drawSidebar(sidebarWidth);
        ImGui::SameLine(0, 0);
        drawContentPanel();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void AppUI::drawSidebarIcon(Panel panel, ImVec2 pos, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool active = (activePanel_ == panel);
    const ImU32 col = active ? IM_COL32(180, 200, 240, 255) : IM_COL32(120, 135, 170, 180);
    float cx = pos.x + size * 0.5f, cy = pos.y + size * 0.5f;
    float r = size * 0.38f;
    float lw = scaled(1.4f);

    switch (panel) {
    case Panel::Resume: {
        ImVec2 a(pos.x + size * 0.28f, pos.y + size * 0.15f);
        ImVec2 b(pos.x + size * 0.82f, cy);
        ImVec2 c(pos.x + size * 0.28f, pos.y + size * 0.85f);
        dl->AddTriangleFilled(a, b, c, col);
        break;
    }
    case Panel::ROM: {
        // Folder icon
        float l = pos.x + size * 0.12f, t = pos.y + size * 0.28f;
        float ri = pos.x + size * 0.88f, bo = pos.y + size * 0.82f;
        dl->AddRectFilled(ImVec2(l, t), ImVec2(ri, bo), col, scaled(2));
        dl->AddRectFilled(ImVec2(l, t - size * 0.1f), ImVec2(l + size * 0.35f, t + scaled(1)), col, scaled(1.5f));
        break;
    }
    case Panel::SaveStates: {
        float l = pos.x + size * 0.18f, t = pos.y + size * 0.12f;
        float ri = pos.x + size * 0.82f, bo = pos.y + size * 0.88f;
        dl->AddRectFilled(ImVec2(l, t), ImVec2(ri, bo), col, scaled(2));
        dl->AddRectFilled(ImVec2(l + size * 0.15f, t), ImVec2(ri - size * 0.08f, t + size * 0.28f),
                          IM_COL32(10, 14, 28, 220), scaled(1));
        dl->AddRectFilled(ImVec2(l + size * 0.18f, bo - size * 0.32f), ImVec2(ri - size * 0.18f, bo - size * 0.06f),
                          IM_COL32(10, 14, 28, 220), scaled(1));
        break;
    }
    case Panel::Settings: {
        for (int i = 0; i < 3; i++) {
            float y = pos.y + size * (0.22f + i * 0.26f);
            float barH = size * 0.09f;
            dl->AddRectFilled(ImVec2(pos.x + size * 0.15f, y), ImVec2(pos.x + size * 0.85f, y + barH), col, barH * 0.5f);
            float dotX = pos.x + size * (0.32f + i * 0.18f);
            dl->AddCircleFilled(ImVec2(dotX, y + barH * 0.5f), barH * 1.3f, col);
        }
        break;
    }
    case Panel::Cheats: {
        dl->AddLine(ImVec2(cx - r * 0.9f, cy), ImVec2(cx - r * 0.15f, cy - r), col, lw);
        dl->AddLine(ImVec2(cx - r * 0.9f, cy), ImVec2(cx - r * 0.15f, cy + r), col, lw);
        dl->AddLine(ImVec2(cx + r * 0.9f, cy), ImVec2(cx + r * 0.15f, cy - r), col, lw);
        dl->AddLine(ImVec2(cx + r * 0.9f, cy), ImVec2(cx + r * 0.15f, cy + r), col, lw);
        break;
    }
    case Panel::Controls: {
        float arm = size * 0.14f;
        dl->AddRectFilled(ImVec2(cx - arm, cy - r), ImVec2(cx + arm, cy + r), col, scaled(1.5f));
        dl->AddRectFilled(ImVec2(cx - r, cy - arm), ImVec2(cx + r, cy + arm), col, scaled(1.5f));
        break;
    }
    case Panel::About: {
        dl->AddCircle(ImVec2(cx, cy), r, col, 16, lw);
        dl->AddCircleFilled(ImVec2(cx, cy - r * 0.38f), r * 0.14f, col);
        dl->AddRectFilled(ImVec2(cx - r * 0.11f, cy - r * 0.08f), ImVec2(cx + r * 0.11f, cy + r * 0.48f), col);
        break;
    }
    }
}

void AppUI::drawSidebar(float sidebarWidth) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.04f, 0.08f, 0.97f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    if (!ImGui::BeginChild("##sidebar", ImVec2(sidebarWidth, 0), ImGuiChildFlags_None,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild(); ImGui::PopStyleVar(2); ImGui::PopStyleColor(); return;
    }

    if (justBecameVisible_) {
        justBecameVisible_ = false;
        ImGui::SetWindowFocus();
    }

    const float pad = scaled(16);
    const float itemH = scaled(38);
    const float iconSize = scaled(16);
    const float topPad = scaled(20);

    // Title
    ImGui::SetCursorPos(ImVec2(pad, topPad));
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(NULL, style.FontSizeBase * 1.35f);
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.00f, 1.00f), "GENESIS");
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + scaled(16));

    // Separator
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wPos = ImGui::GetWindowPos();
    float sepY = wPos.y + ImGui::GetCursorPosY();
    dl->AddLine(ImVec2(wPos.x + pad, sepY), ImVec2(wPos.x + sidebarWidth - pad, sepY),
                IM_COL32(80, 100, 150, 50));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + scaled(12));

    struct NavItem { Panel panel; const char* label; };
    const NavItem items[] = {
        {Panel::Resume,     "Resume"},
        {Panel::ROM,        "Open ROM"},
        {Panel::SaveStates, "Save States"},
        {Panel::Settings,   "Settings"},
        {Panel::Cheats,     "Cheats"},
        {Panel::Controls,   "Controls"},
        {Panel::About,      "About"},
    };

    const ImU32 accentCol = IM_COL32(100, 160, 255, 255);

    for (int i = 0; i < IM_ARRAYSIZE(items); i++) {
        bool selected = (activePanel_ == items[i].panel);
        ImVec2 itemPos(wPos.x, wPos.y + ImGui::GetCursorPosY());
        ImVec2 itemMax(wPos.x + sidebarWidth, itemPos.y + itemH);

        if (selected) {
            dl->AddRectFilled(itemPos, itemMax, IM_COL32(100, 160, 255, 20));
            dl->AddRectFilled(itemPos, ImVec2(itemPos.x + scaled(3), itemMax.y), accentCol, scaled(1.5f));
        }

        // Icon
        drawSidebarIcon(items[i].panel,
                        ImVec2(itemPos.x + pad, itemPos.y + (itemH - iconSize) * 0.5f), iconSize);

        // Label
        ImGui::SetCursorScreenPos(ImVec2(itemPos.x + pad + iconSize + scaled(10),
                                         itemPos.y + (itemH - ImGui::GetTextLineHeight()) * 0.5f));
        if (selected) ImGui::TextUnformatted(items[i].label);
        else ImGui::TextColored(ImVec4(0.58f, 0.62f, 0.72f, 1.0f), "%s", items[i].label);

        // Invisible button for click
        ImGui::SetCursorScreenPos(itemPos);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##nav", ImVec2(sidebarWidth, itemH))) {
            if (items[i].panel == Panel::Resume) {
                visible_ = false;
                Action a; a.type = ActionType::CloseMenu; pushAction(a);
            } else {
                activePanel_ = items[i].panel;
                if (items[i].panel == Panel::ROM && dirEntries_.empty()) refreshDirectory();
            }
        }
        if (ImGui::IsItemHovered() && !selected) {
            dl->AddRectFilled(itemPos, itemMax, IM_COL32(255, 255, 255, 8));
        }
        ImGui::PopID();
    }

    // Quit at bottom
    float quitY = ImGui::GetWindowHeight() - itemH - scaled(12);
    ImVec2 quitPos(wPos.x, wPos.y + quitY);
    ImVec2 quitMax(wPos.x + sidebarWidth, quitPos.y + itemH);

    // Separator before quit
    dl->AddLine(ImVec2(wPos.x + pad, quitPos.y - scaled(6)),
                ImVec2(wPos.x + sidebarWidth - pad, quitPos.y - scaled(6)),
                IM_COL32(80, 100, 150, 40));

    bool quitHov = ImGui::IsMouseHoveringRect(quitPos, quitMax);
    if (quitHov) dl->AddRectFilled(quitPos, quitMax, IM_COL32(200, 50, 50, 25));

    // Power icon
    float qiSize = iconSize;
    ImVec2 qiPos(quitPos.x + pad, quitPos.y + (itemH - qiSize) * 0.5f);
    float qcx = qiPos.x + qiSize * 0.5f, qcy = qiPos.y + qiSize * 0.5f, qr = qiSize * 0.38f;
    ImU32 qcol = quitHov ? IM_COL32(255, 100, 100, 255) : IM_COL32(160, 70, 70, 200);
    dl->AddCircle(ImVec2(qcx, qcy), qr, qcol, 16, scaled(1.4f));
    dl->AddLine(ImVec2(qcx, qcy - qr - scaled(1)), ImVec2(qcx, qcy - qr * 0.15f), qcol, scaled(1.4f));

    ImGui::SetCursorScreenPos(ImVec2(quitPos.x + pad + iconSize + scaled(10),
                                     quitPos.y + (itemH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(quitHov ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) : ImVec4(0.63f, 0.28f, 0.28f, 1.0f), "Quit");

    ImGui::SetCursorScreenPos(quitPos);
    if (ImGui::InvisibleButton("##quit", ImVec2(sidebarWidth, itemH))) {
        Action a; a.type = ActionType::Quit; pushAction(a);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void AppUI::drawContentPanel() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.06f, 0.11f, 0.92f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(scaled(24), scaled(20)));
    if (!ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None)) {
        ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); return;
    }

    switch (activePanel_) {
        case Panel::Resume:     drawResumePanel(); break;
        case Panel::ROM:        drawROMPanel(); break;
        case Panel::SaveStates: drawSaveStatesPanel(); break;
        case Panel::Settings:   drawSettingsPanel(); break;
        case Panel::Cheats:     drawCheatsPanel(); break;
        case Panel::Controls:   drawControlsPanel(); break;
        case Panel::About:      drawAboutPanel(); break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// -----------------------------------------------------------------------
// Resume panel (quick actions)
// -----------------------------------------------------------------------

void AppUI::drawResumePanel() {
    ImGui::Spacing();
    ImGui::Spacing();

    // Now Playing
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(NULL, style.FontSizeBase * 1.5f);
    ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.00f, 1.00f), "Now Playing");
    ImGui::PopFont();
    ImGui::Spacing();

    if (!currentRomName_.empty()) {
        ImGui::PushFont(NULL, style.FontSizeBase * 1.15f);
        ImGui::TextUnformatted(currentRomName_.c_str());
        ImGui::PopFont();
        const char* stdLabel = effectiveVideoStandard_ == VideoStandard::PAL ? "PAL" : "NTSC";
        ImGui::TextDisabled("%s  %.3f Hz  %d lines", stdLabel, effectiveRefreshHz_, effectiveScanlines_);
    } else {
        ImGui::TextDisabled("No ROM loaded");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::Spacing();

    // Resume button
    float buttonW = scaled(200);
    float buttonH = scaled(40);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.68f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.52f, 0.78f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.38f, 0.62f, 1.00f));
    if (ImGui::Button("Resume Game", ImVec2(buttonW, buttonH))) {
        visible_ = false;
        Action a; a.type = ActionType::CloseMenu; pushAction(a);
    }
    ImGui::PopStyleColor(3);

    ImGui::Spacing();
    ImGui::Spacing();

    // Quick actions
    ImGui::TextDisabled("Quick Actions");
    ImGui::Spacing();

    float qButtonW = scaled(140);
    if (ImGui::Button("Save State", ImVec2(qButtonW, 0))) {
        Action a; a.type = ActionType::SaveState; a.intValue = selectedSlot_; pushAction(a);
    }
    ImGui::SameLine();
    bool noSlot = !slotInfo_[selectedSlot_].occupied;
    if (noSlot) ImGui::BeginDisabled();
    if (ImGui::Button("Load State", ImVec2(qButtonW, 0))) {
        Action a; a.type = ActionType::LoadState; a.intValue = selectedSlot_; pushAction(a);
        visible_ = false;
    }
    if (noSlot) ImGui::EndDisabled();

    ImGui::Text("Slot %d", selectedSlot_);

    ImGui::Spacing();
    if (ImGui::Button("Reset Console", ImVec2(qButtonW, 0))) {
        Action a; a.type = ActionType::Reset; pushAction(a);
    }

    // Recent ROMs
    if (!recentRoms_.empty()) {
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextDisabled("Recent");
        ImGui::Spacing();
        for (size_t i = 0; i < recentRoms_.size(); i++) {
            std::filesystem::path romPath(recentRoms_[i].path);
            std::string displayName = romPath.stem().string();
            ImGui::PushID(static_cast<int>(i) + 5000);
            if (ImGui::Selectable(displayName.c_str(), false)) {
                Action a; a.type = ActionType::LoadRom; a.text = recentRoms_[i].path;
                pushAction(a); visible_ = false;
            }
            ImGui::PopID();
        }
    }
}

// -----------------------------------------------------------------------
// ROM panel (recent ROMs + file browser)
// -----------------------------------------------------------------------

void AppUI::refreshDirectory() {
    dirEntries_.clear();
    if (currentDir_.empty()) currentDir_ = std::filesystem::current_path();
    std::error_code ec;
    if (!std::filesystem::is_directory(currentDir_, ec)) currentDir_ = std::filesystem::current_path();
    if (currentDir_.has_parent_path() && currentDir_ != currentDir_.root_path())
        dirEntries_.push_back({"..", true});
    std::vector<DirEntry> dirs, files;
    for (const auto& entry : std::filesystem::directory_iterator(currentDir_, ec)) {
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (entry.is_directory(ec)) {
            dirs.push_back({name, true, 0});
        } else {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".bin" || ext == ".md" || ext == ".gen" || ext == ".smd") {
                std::uintmax_t sz = entry.file_size(ec);
                files.push_back({name, false, ec ? 0 : sz});
            }
        }
    }
    std::sort(dirs.begin(), dirs.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    dirEntries_.insert(dirEntries_.end(), dirs.begin(), dirs.end());
    dirEntries_.insert(dirEntries_.end(), files.begin(), files.end());
}

void AppUI::drawROMPanel() {
    // Path bar
    std::string dirStr = currentDir_.string();
    std::strncpy(pathBarBuf_, dirStr.c_str(), sizeof(pathBarBuf_) - 1);
    pathBarBuf_[sizeof(pathBarBuf_) - 1] = '\0';
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##pathbar", "Enter path...", pathBarBuf_, sizeof(pathBarBuf_),
                                  ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::filesystem::path newDir(pathBarBuf_);
        std::error_code ec;
        if (std::filesystem::is_directory(newDir, ec)) { currentDir_ = newDir; refreshDirectory(); }
    }
    ImGui::Spacing();

    // File list — fills remaining space
    if (ImGui::BeginChild("FileList", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, scaled(80));
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < dirEntries_.size(); i++) {
                const auto& entry = dirEntries_[i];
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                std::string label = entry.isDir ? (">> " + entry.name) : entry.name;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label.c_str(), false,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (entry.isDir) {
                        currentDir_ = (entry.name == "..") ? currentDir_.parent_path() : currentDir_ / entry.name;
                        refreshDirectory();
                    } else {
                        Action a; a.type = ActionType::LoadRom;
                        a.text = (currentDir_ / entry.name).string();
                        pushAction(a); visible_ = false;
                    }
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (!entry.isDir && entry.size > 0) {
                    if (entry.size >= 1024 * 1024) ImGui::Text("%.1f MB", entry.size / (1024.0 * 1024.0));
                    else ImGui::Text("%.1f KB", entry.size / 1024.0);
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

// -----------------------------------------------------------------------
// Save states panel
// -----------------------------------------------------------------------

void AppUI::drawSaveStatesPanel() {
    float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    float listWidth = std::min(scaled(200), ImGui::GetContentRegionAvail().x * 0.48f);

    ImGui::BeginChild("SlotList", ImVec2(listWidth, -footerHeight), ImGuiChildFlags_Borders);
    for (int i = 0; i < 10; i++) {
        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float dotRadius = 4.0f * uiScale_;
        float lineH = ImGui::GetTextLineHeight();
        ImVec2 dotCenter(pos.x + dotRadius + 2.0f, pos.y + lineH * 0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 dotColor = slotInfo_[i].occupied ? UIColors::SlotUsed : UIColors::SlotEmpty;
        dl->AddCircleFilled(dotCenter, dotRadius, ImGui::GetColorU32(dotColor));
        float indent = dotRadius * 2.0f + 8.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        char label[32]; snprintf(label, sizeof(label), "Slot %d", i);
        if (ImGui::Selectable(label, selectedSlot_ == i)) selectedSlot_ = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Preview area
    ImGui::BeginChild("Preview", ImVec2(0, -footerHeight));
    ImGui::Text("Slot %d", selectedSlot_);
    ImGui::Separator();
    if (slotInfo_[selectedSlot_].occupied && slotInfo_[selectedSlot_].glTexture) {
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
        Action a; a.type = ActionType::SaveState; a.intValue = selectedSlot_; pushAction(a);
    }
    ImGui::SameLine();
    bool slotEmpty = !slotInfo_[selectedSlot_].occupied;
    if (slotEmpty) ImGui::BeginDisabled();
    if (ImGui::Button("Load")) {
        Action a; a.type = ActionType::LoadState; a.intValue = selectedSlot_; pushAction(a);
        visible_ = false;
    }
    if (slotEmpty) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && slotEmpty)
        ImGui::SetTooltip("No save data in this slot");
}

// -----------------------------------------------------------------------
// Cheat manager panel
// -----------------------------------------------------------------------

void AppUI::drawCheatsPanel() {
    auto selectedRamResult = [&]() -> const RamSearchResultItem* {
        for (const auto& item : ramSearchResults_)
            if (item.index == ramSearchSelectedIndex_) return &item;
        return nullptr;
    };
    auto selectRamResult = [&](const RamSearchResultItem& item) {
        ramSearchSelectedIndex_ = item.index;
        std::snprintf(ramSearchTargetValueBuf_, sizeof(ramSearchTargetValueBuf_), "%s", item.currentDec.c_str());
        ramSearchTargetNameBuf_[0] = '\0';
    };
    if (ramSearchSelectedIndex_ >= 0 && !selectedRamResult()) {
        ramSearchSelectedIndex_ = -1;
        ramSearchTargetValueBuf_[0] = '\0'; ramSearchTargetNameBuf_[0] = '\0';
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
                Action a; a.type = ActionType::AddCheat; a.text = cheatCodeBuf_; a.text2 = cheatNameBuf_;
                pushAction(a); cheatCodeBuf_[0] = '\0'; cheatNameBuf_[0] = '\0';
            }
            if (codeEmpty) ImGui::EndDisabled();

            ImGui::SeparatorText("Active Cheats");
            float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
            if (ImGui::BeginChild("CheatList", ImVec2(0, -footerHeight), ImGuiChildFlags_Borders)) {
                if (cheats_.empty()) {
                    float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
                    const char* mainText = "No cheats added yet";
                    const char* hintText = "Enter a raw address code above or create one from RAM Search";
                    ImVec2 mainSize = ImGui::CalcTextSize(mainText), hintSize = ImGui::CalcTextSize(hintText);
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
                        Action a; a.type = ActionType::ToggleCheat; a.intValue = static_cast<int>(i); pushAction(a);
                    }
                    ImGui::SameLine();
                    const float removeButtonWidth = ImGui::GetFrameHeight();
                    ImGui::BeginGroup();
                    if (ch.name.empty()) ImGui::TextDisabled("(unnamed)");
                    else ImGui::TextUnformatted(ch.name.c_str());
                    ImGui::TextDisabled("%s", ch.code.c_str());
                    ImGui::EndGroup();
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - removeButtonWidth);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.20f, 0.20f, 0.40f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.30f, 0.30f, 0.60f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.30f, 0.30f, 1.00f));
                    if (ImGui::Button("X", ImVec2(removeButtonWidth, 0))) {
                        Action a; a.type = ActionType::RemoveCheat; a.intValue = static_cast<int>(i); pushAction(a);
                    }
                    ImGui::PopStyleColor(4);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove cheat");
                    ImGui::PopID();
                    if (i + 1 < cheats_.size()) ImGui::Separator();
                }
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::Text("%d cheat(s)", static_cast<int>(cheats_.size()));
            if (!romLoaded_) ImGui::BeginDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Load File")) { Action a; a.type = ActionType::LoadCheatsFromFile; pushAction(a); }
            ImGui::SameLine();
            if (ImGui::Button("Save File")) { Action a; a.type = ActionType::SaveCheatsToFile; pushAction(a); }
            if (!romLoaded_) ImGui::EndDisabled();
            if (!cheats_.empty()) {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - scaled(80));
                ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
                if (ImGui::Button("Clear All")) { confirmClearCheats_ = true; ImGui::OpenPopup("Confirm Clear"); }
                ImGui::PopStyleColor(3);
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("RAM Search")) {
            auto searchTypeLabel = [](Cheats::CheatValueType type) -> const char* { return Cheats::valueTypeLabel(type); };
            auto heuristicLabel = [](Cheats::SearchHeuristic h) -> const char* {
                switch (h) {
                    case Cheats::SearchHeuristic::Off: return "Exact";
                    case Cheats::SearchHeuristic::BooleanLike: return "Boolean / tiny counter";
                    case Cheats::SearchHeuristic::Tolerance1: return "Tolerance +/-1";
                    case Cheats::SearchHeuristic::Tolerance2: return "Tolerance +/-2";
                }
                return "Exact";
            };
            auto emitSearchAction = [&](ActionType type, int iv, int iv2, const char* t, const char* t2 = "") {
                Action a; a.type = type; a.intValue = iv; a.intValue2 = iv2;
                a.text = t ? t : ""; a.text2 = t2 ? t2 : ""; pushAction(a);
            };

            if (!romLoaded_) { ImGui::TextDisabled("%s", "Load a ROM to search writable memory."); ImGui::Spacing(); }
            if (!romLoaded_) ImGui::BeginDisabled();

            ImGui::SeparatorText("Search Setup");
            ImGui::TextDisabled("Mode");
            if (ImGui::RadioButton("Known Value", ramSearchKnownValueMode_)) ramSearchKnownValueMode_ = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Unknown Initial State", !ramSearchKnownValueMode_)) ramSearchKnownValueMode_ = false;

            ImGui::TextDisabled("Value Type");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ram_type", searchTypeLabel(ramSearchType_))) {
                for (int i = 0; i <= static_cast<int>(Cheats::CheatValueType::S32); i++) {
                    auto type = static_cast<Cheats::CheatValueType>(i);
                    bool sel = (type == ramSearchType_);
                    if (ImGui::Selectable(searchTypeLabel(type), sel)) ramSearchType_ = type;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Heuristic Match");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##ram_heuristic", heuristicLabel(ramSearchHeuristic_))) {
                for (int i = 0; i <= static_cast<int>(Cheats::SearchHeuristic::Tolerance2); i++) {
                    auto h = static_cast<Cheats::SearchHeuristic>(i);
                    bool sel = (h == ramSearchHeuristic_);
                    if (ImGui::Selectable(heuristicLabel(h), sel)) ramSearchHeuristic_ = h;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Value");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##ram_value", "Decimal or 0x-prefixed hex", ramSearchValueBuf_, sizeof(ramSearchValueBuf_));

            if (ramSearchKnownValueMode_) {
                if (ImGui::Button("Initial Search", ImVec2(-1, 0)))
                    emitSearchAction(ActionType::StartRamSearchKnownValue, static_cast<int>(ramSearchType_),
                                     static_cast<int>(ramSearchHeuristic_), ramSearchValueBuf_);
            } else {
                if (ImGui::Button("Initial Snapshot", ImVec2(-1, 0)))
                    emitSearchAction(ActionType::StartRamSearchUnknown, static_cast<int>(ramSearchType_), 0, "");
            }
            ImGui::Spacing();
            bool canReset = ramSearchActive_ || ramSearchResultCount_ > 0;
            if (!canReset) ImGui::BeginDisabled();
            if (ImGui::Button("Reset Search", ImVec2(-1, 0))) {
                emitSearchAction(ActionType::ResetRamSearch, 0, 0, "");
                ramSearchSelectedIndex_ = -1; ramSearchTargetValueBuf_[0] = '\0'; ramSearchTargetNameBuf_[0] = '\0';
            }
            if (!canReset) ImGui::EndDisabled();

            ImGui::SeparatorText("Refine");
            if (ramSearchActive_) ImGui::TextDisabled("%d candidate(s)", ramSearchResultCount_);
            else ImGui::TextDisabled("%s", "Take an initial snapshot or known-value search first.");
            if (!ramSearchActive_) ImGui::BeginDisabled();
            if (ImGui::Button("Changed", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::Changed),
                                 static_cast<int>(ramSearchHeuristic_), "");
            if (ImGui::Button("Unchanged", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::Unchanged),
                                 static_cast<int>(ramSearchHeuristic_), "");
            if (ImGui::Button("Increased", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::Increased),
                                 static_cast<int>(ramSearchHeuristic_), "");
            if (ImGui::Button("Decreased", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::Decreased),
                                 static_cast<int>(ramSearchHeuristic_), "");
            ImGui::TextDisabled("Value-Based Refine");
            if (ImGui::Button("Equal To Value", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::EqualToValue),
                                 static_cast<int>(ramSearchHeuristic_), ramSearchValueBuf_);
            if (ImGui::Button("Not Equal To Value", ImVec2(-1, 0)))
                emitSearchAction(ActionType::RefineRamSearch, static_cast<int>(Cheats::SearchCompareMode::NotEqualToValue),
                                 static_cast<int>(ramSearchHeuristic_), ramSearchValueBuf_);
            if (!ramSearchActive_) ImGui::EndDisabled();

            ImGui::SeparatorText("Results");
            ImGui::TextDisabled("%d result(s)%s", ramSearchResultCount_,
                                ramSearchResultCount_ > static_cast<int>(ramSearchResults_.size()) ? ", showing first 500" : "");
            if (ImGui::BeginChild("RamSearchResults", ImVec2(0, scaled(180.0f)), ImGuiChildFlags_Borders)) {
                if (ramSearchResults_.empty()) {
                    ImGui::TextDisabled("%s", ramSearchActive_
                        ? "No matches remain. Reset or change the search criteria."
                        : "No search results yet.");
                } else if (ImGui::BeginTable("RamSearchTable", 5,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Address"); ImGui::TableSetupColumn("Region");
                    ImGui::TableSetupColumn("Current"); ImGui::TableSetupColumn("Hex");
                    ImGui::TableSetupColumn("Previous"); ImGui::TableHeadersRow();
                    for (const auto& item : ramSearchResults_) {
                        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                        char addrText[16]; std::snprintf(addrText, sizeof(addrText), "%06X", item.address & 0xFFFFFFu);
                        bool sel = (item.index == ramSearchSelectedIndex_);
                        if (ImGui::Selectable(addrText, sel, ImGuiSelectableFlags_SpanAllColumns)) selectRamResult(item);
                        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(item.regionLabel.c_str());
                        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(item.currentDec.c_str());
                        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(item.currentHex.c_str());
                        ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(item.previousDec.c_str());
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            ImGui::SeparatorText("Selected Result");
            const RamSearchResultItem* selected = selectedRamResult();
            if (selected) {
                char addrText[16]; std::snprintf(addrText, sizeof(addrText), "%06X", selected->address & 0xFFFFFFu);
                ImGui::Text("Address: %s", addrText);
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
                if (ImGui::Button("Set Once", ImVec2(-1, 0)))
                    emitSearchAction(ActionType::SetRamSearchResultOnce, selected->index, 0, ramSearchTargetValueBuf_);
                if (ImGui::Button("Freeze As Cheat", ImVec2(-1, 0)))
                    emitSearchAction(ActionType::FreezeRamSearchResult, selected->index, 0,
                                     ramSearchTargetValueBuf_, ramSearchTargetNameBuf_);
                if (ImGui::Button("Refresh Value", ImVec2(-1, 0)))
                    emitSearchAction(ActionType::RefreshRamSearchResult, selected->index, 0, "");
            } else {
                ImGui::TextDisabled("%s", "Select a result row to inspect it, set a value once, or freeze it as a cheat.");
            }
            if (!romLoaded_) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // Confirm Clear modal
    ImVec2 modalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Clear", &confirmClearCheats_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove all %d cheats? This cannot be undone.", (int)cheats_.size());
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(scaled(100), 0))) {
            confirmClearCheats_ = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
        if (ImGui::Button("Clear All", ImVec2(scaled(100), 0))) {
            Action a; a.type = ActionType::ClearCheats; pushAction(a);
            confirmClearCheats_ = false; ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------
// Settings panel (unified with sub-tabs)
// -----------------------------------------------------------------------

void AppUI::drawSettingsPanel() {
    if (ImGui::BeginTabBar("SettingsTabs")) {
        if (ImGui::BeginTabItem("Video"))  { drawVideoTab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Audio"))  { drawAudioTab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Input"))  { drawInputTab();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("System")) { drawSystemTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// -----------------------------------------------------------------------
// Video settings tab
// -----------------------------------------------------------------------

void AppUI::drawVideoTab() {
    auto emitBoolAction = [&](ActionType type, bool value) {
        Action a; a.type = type; a.boolValue = value; pushAction(a);
    };
    const float comboWidth = scaled(180);

    ImGui::SeparatorText("Display");
    const bool hasExclusiveModes = !fullscreenResolutions_.empty();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Mode");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##presentation_mode", fullscreenModeLabel(fullscreenMode_))) {
        for (int i = 0; i < 3; i++) {
            auto candidate = static_cast<FullscreenMode>(i);
            bool disabled = (candidate == FullscreenMode::Exclusive && !hasExclusiveModes);
            if (disabled) ImGui::BeginDisabled();
            bool sel = (candidate == fullscreenMode_);
            if (ImGui::Selectable(fullscreenModeLabel(candidate), sel)) {
                fullscreenMode_ = candidate;
                Action a; a.type = ActionType::SetFullscreenMode; a.intValue = i; pushAction(a);
            }
            if (sel) ImGui::SetItemDefaultFocus();
            if (disabled) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }

    if (fullscreenMode_ == FullscreenMode::Windowed) {
        const int windowScales[] = {1, 2, 3, 4, 5, 6};
        char preview[64];
        std::snprintf(preview, sizeof(preview), "%dx (%d x %d)", windowScale_, 320 * windowScale_, 224 * windowScale_);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Window Size");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##window_size", preview)) {
            for (int scale : windowScales) {
                char label[64]; std::snprintf(label, sizeof(label), "%dx (%d x %d)", scale, 320 * scale, 224 * scale);
                bool sel = (scale == windowScale_);
                if (ImGui::Selectable(label, sel)) {
                    windowScale_ = scale;
                    Action a; a.type = ActionType::SetWindowScale; a.intValue = scale; pushAction(a);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    } else if (fullscreenMode_ == FullscreenMode::Exclusive && hasExclusiveModes) {
        const char* selectedLabel = (fullscreenResolutionIndex_ >= 0 &&
                                     fullscreenResolutionIndex_ < static_cast<int>(fullscreenResolutions_.size()))
                                        ? fullscreenResolutions_[fullscreenResolutionIndex_].label.c_str() : "Select a mode";
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Resolution");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
        ImGui::SetNextItemWidth(comboWidth);
        if (ImGui::BeginCombo("##fullscreen_resolution", selectedLabel)) {
            for (int i = 0; i < static_cast<int>(fullscreenResolutions_.size()); i++) {
                bool sel = (i == fullscreenResolutionIndex_);
                if (ImGui::Selectable(fullscreenResolutions_[i].label.c_str(), sel)) {
                    fullscreenResolutionIndex_ = i;
                    Action a; a.type = ActionType::SetFullscreenResolution; a.intValue = i; pushAction(a);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText("Scaling");
    static const char* scalingLabels[] = {"Integer Only", "Fit", "Stretch"};
    int scalingIdx = static_cast<int>(scalingMode_);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Scaling");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##scaling_mode", scalingLabels[scalingIdx])) {
        for (int i = 0; i < 3; i++) {
            bool sel = (i == scalingIdx);
            if (ImGui::Selectable(scalingLabels[i], sel)) {
                scalingMode_ = static_cast<ScalingMode>(i);
                Action a; a.type = ActionType::SetScalingMode; a.intValue = i; pushAction(a);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    static const char* aspectLabels[] = {"Auto", "4:3", "16:9", "Stretch"};
    int aspectIdx = static_cast<int>(aspectRatio_);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Aspect Ratio");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##aspect_ratio", aspectLabels[aspectIdx])) {
        for (int i = 0; i < 4; i++) {
            bool sel = (i == aspectIdx);
            if (ImGui::Selectable(aspectLabels[i], sel)) {
                aspectRatio_ = static_cast<AspectRatio>(i);
                Action a; a.type = ActionType::SetAspectRatio; a.intValue = i; pushAction(a);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    { bool bilinear = bilinearFiltering_;
      if (ImGui::Checkbox("Bilinear Filtering", &bilinear)) {
          bilinearFiltering_ = bilinear; emitBoolAction(ActionType::SetBilinearFiltering, bilinear);
      }
    }

    { bool crt = crtShader_;
      if (ImGui::Checkbox("CRT Shader", &crt)) {
          crtShader_ = crt;
          Action a; a.type = ActionType::SetShaderMode; a.intValue = crt ? 1 : 0;
          pushAction(a);
      }
    }

    if (crtShader_) {
        ImGui::Indent();
        if (ImGui::SliderFloat("Curvature", &crtCurvature_, 0.0f, 8.0f, "%.1f")) {
            Action a; a.type = ActionType::SetShaderMode; a.intValue = 1; pushAction(a);
        }
        if (ImGui::SliderFloat("Scanlines", &crtScanline_, 0.0f, 1.0f, "%.2f")) {
            Action a; a.type = ActionType::SetShaderMode; a.intValue = 1; pushAction(a);
        }
        if (ImGui::SliderFloat("Bloom", &crtBloom_, 0.0f, 0.5f, "%.2f")) {
            Action a; a.type = ActionType::SetShaderMode; a.intValue = 1; pushAction(a);
        }
        if (ImGui::SliderFloat("Vignette", &crtVignette_, 0.0f, 1.0f, "%.2f")) {
            Action a; a.type = ActionType::SetShaderMode; a.intValue = 1; pushAction(a);
        }
        const char* maskTypes[] = { "Aperture Grille", "Slot Mask", "None" };
        if (ImGui::Combo("Phosphor Mask", &crtMaskType_, maskTypes, 3)) {
            Action a; a.type = ActionType::SetShaderMode; a.intValue = 1; pushAction(a);
        }
        ImGui::Unindent();
    }

    ImGui::SeparatorText("Timing");
    { bool vsync = vsyncEnabled_;
      if (ImGui::Checkbox("VSync", &vsync)) { vsyncEnabled_ = vsync; emitBoolAction(ActionType::SetVSync, vsync); }
    }
    { bool limiter = frameLimiterEnabled_;
      if (ImGui::Checkbox("Frame Limiter", &limiter)) {
          frameLimiterEnabled_ = limiter; emitBoolAction(ActionType::SetFrameLimiter, limiter);
      }
    }
    { bool fps = showFPS_;
      if (ImGui::Checkbox("FPS Counter", &fps)) { showFPS_ = fps; emitBoolAction(ActionType::ToggleFPS, fps); }
    }
    if (showFPS_) {
        ImGui::SameLine();
        const float bw = scaled(80);
        auto drawModeBtn = [&](const char* label, ProfilerMode mv) {
            bool sel = (profilerMode_ == mv);
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.47f, 0.62f, 0.92f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.29f, 0.56f, 0.72f, 0.96f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.39f, 0.52f, 0.98f));
            }
            if (ImGui::Button(label, ImVec2(bw, 0))) {
                profilerMode_ = mv;
                Action a; a.type = ActionType::SetProfilerMode; a.intValue = static_cast<int>(mv); pushAction(a);
            }
            if (sel) ImGui::PopStyleColor(3);
            ImGui::SameLine();
        };
        drawModeBtn("Simple", ProfilerMode::Simple);
        drawModeBtn("Detailed", ProfilerMode::Detailed);
        ImGui::NewLine();
    }
}

// -----------------------------------------------------------------------
// Audio settings tab
// -----------------------------------------------------------------------

void AppUI::drawAudioTab() {
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Buffer");
    ImGui::SameLine(); ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##audiobuf", &audioQueueMs_, 50, 500, "%d ms")) {
        Action a; a.type = ActionType::SetAudioQueueMs; a.intValue = audioQueueMs_; pushAction(a);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower = less latency, may crackle. Higher = more stable.");
}

// -----------------------------------------------------------------------
// System (emulation) settings tab
// -----------------------------------------------------------------------

void AppUI::drawSystemTab() {
    static const char* modeLabels[] = {"Auto", "NTSC", "PAL"};
    int modeIndex = static_cast<int>(videoStandardMode_);
    const float comboWidth = scaled(120);

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Video Standard");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth + ImGui::GetCursorPosX());
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##video_standard", modeLabels[modeIndex])) {
        for (int i = 0; i < 3; i++) {
            bool sel = (i == modeIndex);
            if (ImGui::Selectable(modeLabels[i], sel)) {
                videoStandardMode_ = static_cast<VideoStandardMode>(i);
                Action a; a.type = ActionType::SetVideoStandard; a.intValue = i; pushAction(a);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    { bool autoLoad = autoLoadState_;
      if (ImGui::Checkbox("Auto-load save state", &autoLoad)) {
          autoLoadState_ = autoLoad;
          Action a; a.type = ActionType::SetAutoLoadState; a.boolValue = autoLoad; pushAction(a);
      }
    }

    ImGui::Separator();
    const char* activeLabel = effectiveVideoStandard_ == VideoStandard::PAL ? "PAL" : "NTSC";
    ImGui::TextDisabled("%s  %.3f Hz  %d lines", activeLabel, effectiveRefreshHz_, effectiveScanlines_);
}

// -----------------------------------------------------------------------
// Input binding helpers
// -----------------------------------------------------------------------

void AppUI::drawBindingButton(BindableAction action, const ImVec2& size) {
    if (!keyBindings_) return;
    const int actionIndex = static_cast<int>(action);
    const char* keyStr = KeyBindings::keyName(keyBindings_->get(action));
    ImVec2 buttonSize = size;
    if (buttonSize.x <= 0.0f) buttonSize.x = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(actionIndex);
    if (listeningAction_ == actionIndex) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.22f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.28f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.34f, 0.26f, 1.0f));
        ImGui::Button("Press a key...", buttonSize);
        ImGui::PopStyleColor(3);
    } else {
        if (ImGui::Button((keyStr && keyStr[0]) ? keyStr : "(none)", buttonSize))
            listeningAction_ = actionIndex;
    }
    ImGui::PopID();
}

void AppUI::drawBindingGroup(int startIdx, int endIdx) {
    if (ImGui::BeginTable("##bindings", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(150));
        for (int i = startIdx; i < endIdx && i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(KeyBindings::actionName(action));
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            drawBindingButton(action, ImVec2(0.0f, 0.0f));
        }
        ImGui::EndTable();
    }
}

void AppUI::drawControllerBindingDiagram() {
    struct Callout { BindableAction action; const char* label; ImVec2 anchorNorm; ImVec2 fieldNorm; };
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
        ImGui::EndChild(); return;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.y < scaled(360.0f)) canvasSize.y = scaled(360.0f);
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

    const ImVec2 shellMin = pt(0.16f, 0.27f), shellMax = pt(0.84f, 0.77f);
    const float gripRadius = scaled(58.0f);
    draw->AddCircleFilled(pt(0.22f, 0.61f), gripRadius, shellFill, 32);
    draw->AddCircleFilled(pt(0.78f, 0.61f), gripRadius, shellFill, 32);
    draw->AddRectFilled(shellMin, shellMax, shellFill, scaled(32.0f));
    draw->AddRect(shellMin, shellMax, shellEdge, scaled(32.0f), 0, scaled(1.5f));
    draw->AddCircle(pt(0.22f, 0.61f), gripRadius, shellEdge, 32, scaled(1.5f));
    draw->AddCircle(pt(0.78f, 0.61f), gripRadius, shellEdge, 32, scaled(1.5f));

    draw->AddRectFilled(pt(0.26f, 0.43f), pt(0.35f, 0.47f), padFill, scaled(8.0f));
    draw->AddRectFilled(pt(0.285f, 0.39f), pt(0.325f, 0.51f), padFill, scaled(8.0f));

    const ImVec2 startCenter = pt(0.50f, 0.56f), modeCenter = pt(0.50f, 0.47f);
    draw->AddRectFilled(ImVec2(startCenter.x - scaled(22), startCenter.y - scaled(10)),
                        ImVec2(startCenter.x + scaled(22), startCenter.y + scaled(10)), startFill, scaled(10));
    draw->AddRectFilled(ImVec2(modeCenter.x - scaled(18), modeCenter.y - scaled(8)),
                        ImVec2(modeCenter.x + scaled(18), modeCenter.y + scaled(8)),
                        IM_COL32(88, 96, 130, 255), scaled(8));

    const float buttonRadius = scaled(14.0f);
    draw->AddCircleFilled(pt(0.65f, 0.41f), buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(pt(0.73f, 0.38f), buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(pt(0.81f, 0.41f), buttonRadius, actionFillAlt, 24);
    draw->AddCircleFilled(pt(0.66f, 0.55f), buttonRadius, actionFill, 24);
    draw->AddCircleFilled(pt(0.74f, 0.52f), buttonRadius, actionFill, 24);
    draw->AddCircleFilled(pt(0.82f, 0.55f), buttonRadius, actionFill, 24);

    static const Callout callouts[] = {
        {BindableAction::PadUp,      "Up",    ImVec2(0.305f, 0.39f), ImVec2(0.03f, 0.06f)},
        {BindableAction::PadLeft,    "Left",  ImVec2(0.26f, 0.45f),  ImVec2(0.03f, 0.28f)},
        {BindableAction::PadDown,    "Down",  ImVec2(0.305f, 0.51f), ImVec2(0.03f, 0.50f)},
        {BindableAction::PadRight,   "Right", ImVec2(0.35f, 0.45f),  ImVec2(0.03f, 0.72f)},
        {BindableAction::ButtonMode, "Mode",  ImVec2(0.50f, 0.47f),  ImVec2(0.36f, 0.80f)},
        {BindableAction::ButtonStart,"Start", ImVec2(0.50f, 0.56f),  ImVec2(0.50f, 0.80f)},
        {BindableAction::ButtonX,    "X",     ImVec2(0.65f, 0.41f),  ImVec2(0.82f, 0.08f)},
        {BindableAction::ButtonY,    "Y",     ImVec2(0.73f, 0.38f),  ImVec2(0.82f, 0.24f)},
        {BindableAction::ButtonZ,    "Z",     ImVec2(0.81f, 0.41f),  ImVec2(0.82f, 0.40f)},
        {BindableAction::ButtonA,    "A",     ImVec2(0.66f, 0.55f),  ImVec2(0.82f, 0.56f)},
        {BindableAction::ButtonB,    "B",     ImVec2(0.74f, 0.52f),  ImVec2(0.82f, 0.72f)},
        {BindableAction::ButtonC,    "C",     ImVec2(0.82f, 0.55f),  ImVec2(0.82f, 0.88f)},
    };

    const float labelHeight = ImGui::GetTextLineHeight();
    for (const Callout& co : callouts) {
        ImVec2 labelPos(canvas.Min.x + canvasSize.x * co.fieldNorm.x,
                        canvas.Min.y + canvasSize.y * co.fieldNorm.y);
        if (co.fieldNorm.x > 0.75f) labelPos.x = canvas.Max.x - fieldWidth - scaled(12);
        else if (co.fieldNorm.x > 0.34f && co.fieldNorm.x < 0.7f) labelPos.x -= fieldWidth * 0.5f;
        if (labelPos.x < canvas.Min.x + scaled(12)) labelPos.x = canvas.Min.x + scaled(12);

        ImVec2 fieldPos(labelPos.x, labelPos.y + labelHeight + labelGap);
        const ImVec2 anchor = pt(co.anchorNorm.x, co.anchorNorm.y);
        const ImRect fieldRect(fieldPos, ImVec2(fieldPos.x + fieldWidth, fieldPos.y + fieldHeight));

        ImVec2 target;
        if (fieldRect.Max.x < anchor.x) target = ImVec2(fieldRect.Max.x, fieldRect.GetCenter().y);
        else if (fieldRect.Min.x > anchor.x) target = ImVec2(fieldRect.Min.x, fieldRect.GetCenter().y);
        else target = ImVec2(fieldRect.GetCenter().x, fieldRect.Min.y);

        float midX = (anchor.x + target.x) * 0.5f;
        draw->AddCircleFilled(anchor, scaled(3), accentLine, 12);
        draw->AddLine(anchor, ImVec2(midX, anchor.y), accentLine, scaled(1.5f));
        draw->AddLine(ImVec2(midX, anchor.y), ImVec2(midX, target.y), accentLine, scaled(1.5f));
        draw->AddLine(ImVec2(midX, target.y), target, accentLine, scaled(1.5f));

        ImGui::SetCursorScreenPos(labelPos);
        ImGui::TextUnformatted(co.label);
        ImGui::SetCursorScreenPos(fieldPos);
        drawBindingButton(co.action, ImVec2(fieldWidth, fieldHeight));
    }

    ImGui::SetCursorScreenPos(ImVec2(canvas.Min.x, canvas.Max.y));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
}

// -----------------------------------------------------------------------
// Input settings tab
// -----------------------------------------------------------------------

void AppUI::drawInputTab() {
    if (!keyBindings_) { ImGui::Text("No keybindings available"); return; }

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
        confirmResetBindings_ = true; ImGui::OpenPopup("Confirm Reset");
    }

    ImVec2 modalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Confirm Reset", &confirmResetBindings_, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Reset all key bindings to defaults?");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(scaled(100), 0))) {
            confirmResetBindings_ = false; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, UIColors::Danger);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UIColors::DangerHov);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UIColors::DangerAct);
        if (ImGui::Button("Reset", ImVec2(scaled(100), 0))) {
            keyBindings_->loadDefaults(); keyBindings_->save();
            Action a; a.type = ActionType::SetKeyBinding; pushAction(a);
            confirmResetBindings_ = false; ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------
// About panel
// -----------------------------------------------------------------------

void AppUI::drawAboutPanel() {
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
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::Text("Coded by pagefault");
}

// -----------------------------------------------------------------------
// Controls panel
// -----------------------------------------------------------------------

void AppUI::drawControlsPanel() {
    if (!keyBindings_) { ImGui::Text("No keybindings loaded"); return; }

    ImGui::SeparatorText("Gamepad");
    if (ImGui::BeginTable("controls_pad", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(140));
        for (int i = 0; i < 12 && i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(KeyBindings::actionName(action));
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::Text("%s", KeyBindings::keyName(keyBindings_->get(action)));
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();

    ImGui::SeparatorText("Hotkeys");
    if (ImGui::BeginTable("controls_hotkeys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, scaled(140));
        for (int i = 12; i < KeyBindings::actionCount(); i++) {
            auto action = static_cast<BindableAction>(i);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(KeyBindings::actionName(action));
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            ImGui::Text("%s", KeyBindings::keyName(keyBindings_->get(action)));
        }
        ImGui::EndTable();
    }
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
