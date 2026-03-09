// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/keybindings.h"
#include "config_dir.h"

#include <SDL3/SDL_keyboard.h>
#include <fstream>
#include <string>

void KeyBindings::loadDefaults() {
    bindings[static_cast<int>(BindableAction::PadUp)]            = SDLK_UP;
    bindings[static_cast<int>(BindableAction::PadDown)]          = SDLK_DOWN;
    bindings[static_cast<int>(BindableAction::PadLeft)]          = SDLK_LEFT;
    bindings[static_cast<int>(BindableAction::PadRight)]         = SDLK_RIGHT;
    bindings[static_cast<int>(BindableAction::ButtonA)]          = SDLK_Z;
    bindings[static_cast<int>(BindableAction::ButtonB)]          = SDLK_X;
    bindings[static_cast<int>(BindableAction::ButtonC)]          = SDLK_C;
    bindings[static_cast<int>(BindableAction::ButtonStart)]      = SDLK_RETURN;
    bindings[static_cast<int>(BindableAction::ButtonX)]          = SDLK_A;
    bindings[static_cast<int>(BindableAction::ButtonY)]          = SDLK_W;
    bindings[static_cast<int>(BindableAction::ButtonZ)]          = SDLK_D;
    bindings[static_cast<int>(BindableAction::ButtonMode)]       = SDLK_Q;
    bindings[static_cast<int>(BindableAction::OpenMenu)]         = SDLK_ESCAPE;
    bindings[static_cast<int>(BindableAction::QuickSave)]        = SDLK_F5;
    bindings[static_cast<int>(BindableAction::QuickLoad)]        = SDLK_F7;
    bindings[static_cast<int>(BindableAction::SaveStatePicker)]  = SDLK_F6;
    bindings[static_cast<int>(BindableAction::ToggleFPS)]        = SDLK_F12;
    bindings[static_cast<int>(BindableAction::ToggleFrameLimiter)] = SDLK_T;
    bindings[static_cast<int>(BindableAction::Pause)]            = SDLK_SPACE;
    bindings[static_cast<int>(BindableAction::Quit)]             = SDLK_F10;
    bindings[static_cast<int>(BindableAction::FrameStep)]       = SDLK_F;
    bindings[static_cast<int>(BindableAction::SingleStep)]      = SDLK_S;
    bindings[static_cast<int>(BindableAction::FastForward)]     = SDLK_TAB;
    bindings[static_cast<int>(BindableAction::NextSlot)]        = SDLK_F8;
    bindings[static_cast<int>(BindableAction::PrevSlot)]        = SDLK_F4;
}

bool KeyBindings::load() {
    loadDefaults();
    std::filesystem::path file = getConfigDir() / "keybindings.txt";
    std::ifstream in(file);
    if (!in.is_open()) return false;

    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        int keycode = 0;
        try { keycode = std::stoi(line.substr(eq + 1)); } catch (...) { continue; }

        for (int i = 0; i < static_cast<int>(BindableAction::COUNT); i++) {
            if (name == actionName(static_cast<BindableAction>(i))) {
                bindings[i] = static_cast<SDL_Keycode>(keycode);
                break;
            }
        }
    }
    return true;
}

bool KeyBindings::save() {
    std::filesystem::path file = getConfigDir() / "keybindings.txt";
    std::ofstream out(file);
    if (!out.is_open()) return false;
    for (int i = 0; i < static_cast<int>(BindableAction::COUNT); i++) {
        out << actionName(static_cast<BindableAction>(i)) << "=" << static_cast<int>(bindings[i]) << "\n";
    }
    return true;
}

const char* KeyBindings::actionName(BindableAction a) {
    switch (a) {
        case BindableAction::PadUp:              return "PadUp";
        case BindableAction::PadDown:            return "PadDown";
        case BindableAction::PadLeft:            return "PadLeft";
        case BindableAction::PadRight:           return "PadRight";
        case BindableAction::ButtonA:            return "ButtonA";
        case BindableAction::ButtonB:            return "ButtonB";
        case BindableAction::ButtonC:            return "ButtonC";
        case BindableAction::ButtonStart:        return "ButtonStart";
        case BindableAction::ButtonX:            return "ButtonX";
        case BindableAction::ButtonY:            return "ButtonY";
        case BindableAction::ButtonZ:            return "ButtonZ";
        case BindableAction::ButtonMode:         return "ButtonMode";
        case BindableAction::OpenMenu:           return "OpenMenu";
        case BindableAction::QuickSave:          return "QuickSave";
        case BindableAction::QuickLoad:          return "QuickLoad";
        case BindableAction::SaveStatePicker:    return "SaveStatePicker";
        case BindableAction::ToggleFPS:          return "ToggleFPS";
        case BindableAction::ToggleFrameLimiter: return "ToggleFrameLimiter";
        case BindableAction::Pause:              return "Pause";
        case BindableAction::Quit:               return "Quit";
        case BindableAction::FrameStep:          return "FrameStep";
        case BindableAction::SingleStep:         return "SingleStep";
        case BindableAction::FastForward:        return "FastForward";
        case BindableAction::NextSlot:           return "NextSlot";
        case BindableAction::PrevSlot:           return "PrevSlot";
        case BindableAction::COUNT:              return "?";
    }
    return "?";
}

const char* KeyBindings::keyName(SDL_Keycode key) {
    return SDL_GetKeyName(key);
}
