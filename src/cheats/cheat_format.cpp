// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/cheat_format.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace Cheats {
namespace {

std::string trimCopy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        first++;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        last--;
    }
    return s.substr(first, last - first);
}

std::string stripHexPrefix(const std::string& s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return s.substr(2);
    }
    if (!s.empty() && s[0] == '$') {
        return s.substr(1);
    }
    return s;
}

bool parseHex(const std::string& s, u32& out) {
    if (s.empty()) {
        return false;
    }

    u32 value = 0;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isxdigit(uc)) {
            return false;
        }
        int nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else nibble = 10 + (c - 'A');
        value = (value << 4) | static_cast<u32>(nibble);
    }
    out = value;
    return true;
}

} // namespace

bool parseRawCheatCode(const std::string& input, CheatEntry& outEntry, std::string* errorText) {
    const std::string text = trimCopy(input);
    if (text.empty()) {
        if (errorText) *errorText = "Cheat code is empty";
        return false;
    }

    size_t sep = text.find(':');
    if (sep == std::string::npos) sep = text.find('=');
    if (sep == std::string::npos) sep = text.find(' ');
    if (sep == std::string::npos) {
        if (errorText) *errorText = "Use format FFFFFF:YY, FFFFFF:YYYY, or FFFFFF:XXXXXXXX";
        return false;
    }

    const std::string addrText = stripHexPrefix(trimCopy(text.substr(0, sep)));
    const std::string valueText = stripHexPrefix(trimCopy(text.substr(sep + 1)));
    if (addrText.empty() || valueText.empty()) {
        if (errorText) *errorText = "Address and value are required";
        return false;
    }
    if (addrText.size() > 6) {
        if (errorText) *errorText = "Address must be up to 6 hex digits";
        return false;
    }
    if (valueText.size() != 2 && valueText.size() != 4 && valueText.size() != 8) {
        if (errorText) *errorText = "Value must be 2, 4, or 8 hex digits";
        return false;
    }

    u32 addr = 0;
    u32 value = 0;
    if (!parseHex(addrText, addr)) {
        if (errorText) *errorText = "Invalid address hex";
        return false;
    }
    if (!parseHex(valueText, value)) {
        if (errorText) *errorText = "Invalid value hex";
        return false;
    }
    if (addr > 0xFFFFFFu) {
        if (errorText) *errorText = "Address out of 24-bit range";
        return false;
    }

    CheatValueType type = CheatValueType::U8;
    if (valueText.size() == 4) type = CheatValueType::U16;
    else if (valueText.size() == 8) type = CheatValueType::U32;

    if ((type == CheatValueType::U16 || type == CheatValueType::U32) && (addr & 1u)) {
        if (errorText) *errorText = "16-bit and 32-bit cheats require even address";
        return false;
    }

    outEntry.address = addr;
    outEntry.value = value;
    outEntry.type = type;
    outEntry.mode = CheatMode::Freeze;
    outEntry.enabled = true;
    outEntry.code = formatRawCheatCode(addr, value, type);
    return true;
}

std::string formatRawCheatCode(u32 address, u32 value, CheatValueType type) {
    char buffer[32];
    switch (type) {
        case CheatValueType::U8:
        case CheatValueType::S8:
            std::snprintf(buffer, sizeof(buffer), "%06X:%02X", address & 0xFFFFFFu, value & 0xFFu);
            break;
        case CheatValueType::U16:
        case CheatValueType::S16:
            std::snprintf(buffer, sizeof(buffer), "%06X:%04X", address & 0xFFFFFFu, value & 0xFFFFu);
            break;
        case CheatValueType::U32:
        case CheatValueType::S32:
            std::snprintf(buffer, sizeof(buffer), "%06X:%08X", address & 0xFFFFFFu, value);
            break;
    }
    return buffer;
}

bool parseSearchValueText(const std::string& input, s32& outValue) {
    const std::string trimmed = trimCopy(input);
    if (trimmed.empty()) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(trimmed.c_str(), &end, 0);
    if (end == trimmed.c_str() || *end != '\0' || errno != 0) {
        return false;
    }
    outValue = static_cast<s32>(value);
    return true;
}

} // namespace Cheats
