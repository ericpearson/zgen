// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include "cheats/cheat_types.h"
#include "ui/keybindings.h"
#include "ui/game_renderer.h"
#include <string>
#include <vector>
#include <filesystem>

struct SDL_Window;
union SDL_Event;
struct ImVec2;

enum class FullscreenMode { Windowed, BorderlessDesktop, Exclusive };
enum class ProfilerMode { Simple, Detailed };

// All ImGui UI for the emulator: menu bar, windows, style, actions.
// Replaces the old pixel-drawn MenuSystem.

class AppUI {
public:
    // Actions the UI can request from main loop
    enum class ActionType {
        None,
        Quit,
        Reset,
        LoadRom,
        SetFrameLimiter,
        SetWindowScale,
        SetAudioQueueMs,
        ToggleFPS,
        SetProfilerMode,
        AddCheat,
        ToggleCheat,
        RemoveCheat,
        ClearCheats,
        LoadCheatsFromFile,
        SaveCheatsToFile,
        StartRamSearchKnownValue,
        StartRamSearchUnknown,
        RefineRamSearch,
        ResetRamSearch,
        SetRamSearchResultOnce,
        FreezeRamSearchResult,
        RefreshRamSearchResult,
        SaveState,
        LoadState,
        SetKeyBinding,
        SetFullscreenMode,
        SetFullscreenResolution,
        SetScalingMode,
        SetAspectRatio,
        SetVideoStandard,
        SetVSync,
        SetBilinearFiltering,
        SetAutoLoadState,
        CloseMenu,
    };

    struct Action {
        ActionType type = ActionType::None;
        std::string text;
        std::string text2;
        bool boolValue = false;
        int intValue = 0;
        int intValue2 = 0;
    };

    struct CheatItem {
        std::string code;
        std::string name;
        bool enabled = false;
    };

    struct RamSearchResultItem {
        int index = -1;
        u32 address = 0;
        Cheats::SearchRegionKind region = Cheats::SearchRegionKind::MainRam;
        std::string regionLabel;
        std::string currentDec;
        std::string currentHex;
        std::string previousDec;
        std::string previousHex;
        std::string typeLabel;
    };

    struct FullscreenResolution {
        int width = 0;
        int height = 0;
        int refreshHz = 0;
        std::string label;
    };

    struct PerformanceStats {
        int fps = 0;
        float frameMs = 0.0f;
        float busyMs = 0.0f;
        float sdlMs = 0.0f;
        float emulationMs = 0.0f;
        float cheatsMs = 0.0f;
        float m68kMs = 0.0f;
        float z80Ms = 0.0f;
        float vdpMs = 0.0f;
        float ymMs = 0.0f;
        float psgMs = 0.0f;
        float mixMs = 0.0f;
        float emulationOtherMs = 0.0f;
        float audioMs = 0.0f;
        float videoMs = 0.0f;
        float uiMs = 0.0f;
        float presentMs = 0.0f;
        float pacingMs = 0.0f;
        float otherMs = 0.0f;
    };

    // Init ImGui context, fonts, style. Call once after GL context.
    void init(SDL_Window* window, void* glContext);

    // Shut down ImGui.
    void shutdown();

    // Process an SDL event. Returns true if ImGui consumed it.
    bool processEvent(const SDL_Event& event);

    // Begin a new ImGui frame.
    void newFrame();

    // Draw all UI. Returns an action if user triggered one.
    Action draw();

    // Render ImGui draw data to screen.
    void render();

    // Visibility
    bool isVisible() const { return visible_; }
    void setVisible(bool v) {
        if (v && !visible_) justBecameVisible_ = true;
        visible_ = v;
    }
    void toggleVisible() { setVisible(!visible_); }

    // State setters from main loop
    void setKeyBindings(KeyBindings* kb) { keyBindings_ = kb; }
    void setFrameLimiter(bool v) { frameLimiterEnabled_ = v; }
    void setWindowScale(int s) { windowScale_ = s; }
    void setAudioQueueMs(int ms) { audioQueueMs_ = ms; }
    void setShowFPS(bool v) { showFPS_ = v; }
    void setProfilerMode(ProfilerMode mode) { profilerMode_ = mode; }
    void setFullscreenMode(FullscreenMode mode) { fullscreenMode_ = mode; }
    void setScalingMode(ScalingMode m) { scalingMode_ = m; }
    void setAspectRatio(AspectRatio a) { aspectRatio_ = a; }
    void setVideoStandardMode(VideoStandardMode mode) { videoStandardMode_ = mode; }
    void setEffectiveVideoTiming(VideoStandard standard, double refreshHz, int scanlines);
    void setVSync(bool v) { vsyncEnabled_ = v; }
    void setBilinearFiltering(bool v) { bilinearFiltering_ = v; }
    void setFullscreenResolutions(const std::vector<FullscreenResolution>& modes,
                                  int selectedIndex,
                                  const std::string& displayName);
    void setAutoLoadState(bool v) { autoLoadState_ = v; }
    void setRomLoaded(bool loaded) { romLoaded_ = loaded; }

    void openSaveStates() { showSaveStates_ = true; visible_ = true; }
    void openFileBrowser() { showFileBrowser_ = true; visible_ = true; refreshDirectory(); }

    ScalingMode getScalingMode() const { return scalingMode_; }
    AspectRatio getAspectRatio() const { return aspectRatio_; }
    void setCurrentRomPath(const std::string& p);
    void setCheats(const std::vector<CheatItem>& c) { cheats_ = c; }
    void setRamSearchActive(bool active) { ramSearchActive_ = active; }
    void setRamSearchResultCount(int count) { ramSearchResultCount_ = count; }
    void setRamSearchResults(const std::vector<RamSearchResultItem>& results) { ramSearchResults_ = results; }
    void setStatus(const std::string& text);

    void addRecentRom(const std::string& path);

    // Save state slot info
    struct SlotInfo {
        bool occupied = false;
        unsigned int glTexture = 0; // GL texture for thumbnail
    };
    void setSlotInfo(int slot, bool occupied, const u32* thumbnail = nullptr);

    // FPS overlay (drawn even when menu hidden)
    void drawFPSOverlay(const PerformanceStats& stats, ProfilerMode mode);

    // Status bar overlay
    void drawStatusOverlay();

    // Whether ImGui wants keyboard/mouse
    bool wantCaptureKeyboard() const;
    bool wantCaptureMouse() const;

    // Key rebinding: returns true if UI is listening for a key press.
    // Main loop should call feedKeyForRebind() with the SDL_Keycode.
    bool isListeningForKey() const { return listeningAction_ >= 0; }
    void feedKeyForRebind(SDL_Keycode key);

private:
    bool visible_ = false;
    bool justBecameVisible_ = false;
    KeyBindings* keyBindings_ = nullptr;

    // Runtime state mirrors
    bool frameLimiterEnabled_ = true;
    int windowScale_ = 2;
    int audioQueueMs_ = 250;
    bool showFPS_ = false;
    ProfilerMode profilerMode_ = ProfilerMode::Simple;
    FullscreenMode fullscreenMode_ = FullscreenMode::Windowed;
    ScalingMode scalingMode_ = ScalingMode::IntegerOnly;
    AspectRatio aspectRatio_ = AspectRatio::Auto;
    VideoStandardMode videoStandardMode_ = VideoStandardMode::Auto;
    VideoStandard effectiveVideoStandard_ = VideoStandard::NTSC;
    double effectiveRefreshHz_ = 0.0;
    int effectiveScanlines_ = 262;
    bool vsyncEnabled_ = false;
    bool bilinearFiltering_ = false;
    bool autoLoadState_ = false;
    std::vector<FullscreenResolution> fullscreenResolutions_;
    int fullscreenResolutionIndex_ = -1;
    std::string fullscreenDisplayName_;

    // Status bar
    std::string statusText_;
    float statusTimer_ = 0.0f;

    // Cheats
    std::vector<CheatItem> cheats_;
    char cheatCodeBuf_[64] = {};
    char cheatNameBuf_[64] = {};
    bool romLoaded_ = false;
    bool ramSearchKnownValueMode_ = true;
    Cheats::CheatValueType ramSearchType_ = Cheats::CheatValueType::U16;
    Cheats::SearchHeuristic ramSearchHeuristic_ = Cheats::SearchHeuristic::Off;
    int ramSearchSelectedIndex_ = -1;
    int ramSearchResultCount_ = 0;
    bool ramSearchActive_ = false;
    std::vector<RamSearchResultItem> ramSearchResults_;
    char ramSearchValueBuf_[64] = {};
    char ramSearchTargetValueBuf_[64] = {};
    char ramSearchTargetNameBuf_[64] = {};

    // File browser
    bool showFileBrowser_ = false;
    std::filesystem::path currentDir_;
    struct DirEntry {
        std::string name;
        bool isDir;
        std::uintmax_t size = 0;
    };
    char pathBarBuf_[1024] = {};
    std::vector<DirEntry> dirEntries_;
    void refreshDirectory();

    // Save states
    bool showSaveStates_ = false;
    int selectedSlot_ = 0;
    SlotInfo slotInfo_[10] = {};

    // Settings windows
    bool showVideoSettings_ = false;
    bool showAudioSettings_ = false;
    bool showEmuSettings_ = false;
    bool showInputSettings_ = false;
    bool showAbout_ = false;
    bool showControls_ = false;
    bool showCheatManager_ = false;

    // DPI scaling
    SDL_Window* window_ = nullptr;
    float physicalDpiScale_ = 1.0f;
    float layoutScale_ = 1.0f;
    float uiScale_ = 1.0f;
    int lastWindowWidth_ = 0;
    int lastWindowHeight_ = 0;
    int lastDrawableWidth_ = 0;
    int lastDrawableHeight_ = 0;
    float scaled(float px) const { return px * uiScale_; }
    void prepareCenteredWindow(const ImVec2& desiredSize,
                               const ImVec2& minSize,
                               const ImVec2& maxSize);

    // Confirmation dialogs
    bool confirmClearCheats_ = false;
    bool confirmResetBindings_ = false;

    // Input rebinding
    int listeningAction_ = -1; // which action is waiting for a key press

    // Recent ROMs
    struct RecentRom { std::string path; std::string name; };
    std::vector<RecentRom> recentRoms_;
    void loadRecentRoms();
    void saveRecentRoms();

    // Pending action (accumulated during draw)
    Action pending_;
    void pushAction(Action a);

    // Drawing helpers
    void drawMenuBar();
    void drawFileBrowser();
    void drawSaveStates();
    void drawCheatManager();
    void drawVideoSettings();
    void drawAudioSettings();
    void drawEmuSettings();
    void drawInputSettings();
    void drawAbout();
    void drawControls();
    void drawBindingGroup(int startIdx, int endIdx);
    void drawBindingButton(BindableAction action, const ImVec2& size);
    void drawControllerBindingDiagram();

    // Style
    void applyStyle();
    void updateScaleMetrics(bool force = false);

    static std::filesystem::path getConfigDir();
};
