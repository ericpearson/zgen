// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <SDL3/SDL_keycode.h>
#include <string>

enum class BindableAction {
    // Game controller (Player 1)
    PadUp, PadDown, PadLeft, PadRight,
    ButtonA, ButtonB, ButtonC, ButtonStart,
    ButtonX, ButtonY, ButtonZ, ButtonMode,
    // Emulator hotkeys
    OpenMenu, QuickSave, QuickLoad, SaveStatePicker,
    ToggleFPS, ToggleFrameLimiter, Pause, Quit,
    FrameStep, SingleStep, FastForward,
    NextSlot, PrevSlot,
    COUNT
};

struct KeyBindings {
    SDL_Keycode bindings[static_cast<int>(BindableAction::COUNT)];

    void loadDefaults();
    bool load();   // from ~/.genesis/keybindings.txt
    bool save();   // to ~/.genesis/keybindings.txt

    SDL_Keycode get(BindableAction a) const {
        return bindings[static_cast<int>(a)];
    }

    void set(BindableAction a, SDL_Keycode key) {
        bindings[static_cast<int>(a)] = key;
    }

    static const char* actionName(BindableAction a);
    static const char* keyName(SDL_Keycode key);
    static int actionCount() { return static_cast<int>(BindableAction::COUNT); }
};
