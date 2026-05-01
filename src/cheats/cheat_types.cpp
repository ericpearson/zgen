// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/cheat_types.h"

#include <cstdio>

namespace Cheats {

const char* valueTypeLabel(CheatValueType type) {
    switch (type) {
        case CheatValueType::U8:  return "u8";
        case CheatValueType::S8:  return "s8";
        case CheatValueType::U16: return "u16";
        case CheatValueType::S16: return "s16";
        case CheatValueType::U32: return "u32";
        case CheatValueType::S32: return "s32";
    }
    return "u16";
}

const char* regionLabel(SearchRegionKind region) {
    switch (region) {
        case SearchRegionKind::MainRam: return "68K RAM";
        case SearchRegionKind::Z80Ram:  return "Z80 RAM";
        case SearchRegionKind::SRAM:    return "SRAM";
    }
    return "RAM";
}

int valueTypeByteWidth(CheatValueType type) {
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8:
            return 1;
        case CheatValueType::U16:
        case CheatValueType::S16:
            return 2;
        case CheatValueType::U32:
        case CheatValueType::S32:
            return 4;
    }
    return 1;
}

bool isSignedType(CheatValueType type) {
    return type == CheatValueType::S8 ||
           type == CheatValueType::S16 ||
           type == CheatValueType::S32;
}

s32 reinterpretSigned(u32 rawValue, CheatValueType type) {
    switch (type) {
        case CheatValueType::S8:
            return static_cast<s8>(rawValue & 0xFFu);
        case CheatValueType::S16:
            return static_cast<s16>(rawValue & 0xFFFFu);
        case CheatValueType::S32:
            return static_cast<s32>(rawValue);
        case CheatValueType::U8:
            return static_cast<s32>(rawValue & 0xFFu);
        case CheatValueType::U16:
            return static_cast<s32>(rawValue & 0xFFFFu);
        case CheatValueType::U32:
            return static_cast<s32>(rawValue);
    }
    return 0;
}

std::string formatValueDecimal(u32 rawValue, CheatValueType type) {
    char buffer[32];
    if (isSignedType(type)) {
        std::snprintf(buffer, sizeof(buffer), "%d", reinterpretSigned(rawValue, type));
    } else {
        switch (type) {
            case CheatValueType::U8:
                std::snprintf(buffer, sizeof(buffer), "%u", rawValue & 0xFFu);
                break;
            case CheatValueType::U16:
                std::snprintf(buffer, sizeof(buffer), "%u", rawValue & 0xFFFFu);
                break;
            case CheatValueType::U32:
                std::snprintf(buffer, sizeof(buffer), "%u", rawValue);
                break;
            default:
                std::snprintf(buffer, sizeof(buffer), "%d", reinterpretSigned(rawValue, type));
                break;
        }
    }
    return buffer;
}

std::string formatValueHex(u32 rawValue, CheatValueType type) {
    char buffer[32];
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8:
            std::snprintf(buffer, sizeof(buffer), "0x%02X", rawValue & 0xFFu);
            break;
        case CheatValueType::U16:
        case CheatValueType::S16:
            std::snprintf(buffer, sizeof(buffer), "0x%04X", rawValue & 0xFFFFu);
            break;
        case CheatValueType::U32:
        case CheatValueType::S32:
            std::snprintf(buffer, sizeof(buffer), "0x%08X", rawValue);
            break;
    }
    return buffer;
}

} // namespace Cheats
