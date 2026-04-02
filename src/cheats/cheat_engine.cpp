// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/cheat_engine.h"

#include <cstdio>

namespace Cheats {

namespace {

bool addressFitsRange(u32 address, CheatValueType type, const MemoryRange& range) {
    const int width = valueTypeByteWidth(type);
    if (address < range.start || address > range.end) {
        return false;
    }
    const u32 last = address + static_cast<u32>(width - 1);
    return last >= address && last <= range.end;
}

std::string defaultSearchCheatName(u32 address, CheatValueType type) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "RAM %06X (%s)", address & 0xFFFFFFu, valueTypeLabel(type));
    return buffer;
}

} // namespace

CheatEngine::CheatEngine(CheatMemoryInterface& memory)
    : memory_(memory) {
}

bool CheatEngine::addRawCheat(const std::string& code, const std::string& name, std::string* errorText) {
    CheatEntry parsed;
    if (!parseRawCheatCode(code, parsed, errorText)) {
        return false;
    }

    if (!name.empty()) {
        parsed.name = name;
    } else {
        parsed.name = "Cheat " + std::to_string(cheats_.size() + 1);
    }
    return addOrUpdateCheat(parsed, errorText);
}

bool CheatEngine::addOrUpdateCheat(const CheatEntry& cheat, std::string* errorText) {
    if (!validateCheatEntry(cheat, errorText)) {
        return false;
    }

    for (CheatEntry& existing : cheats_) {
        if (existing.address == cheat.address &&
            existing.type == cheat.type &&
            existing.mode == cheat.mode) {
            existing.value = cheat.value;
            existing.code = cheat.code;
            existing.enabled = true;
            if (!cheat.name.empty()) {
                existing.name = cheat.name;
            }
            return true;
        }
    }

    cheats_.push_back(cheat);
    return true;
}

bool CheatEngine::toggleCheat(size_t index) {
    if (index >= cheats_.size()) {
        return false;
    }
    cheats_[index].enabled = !cheats_[index].enabled;
    return true;
}

bool CheatEngine::removeCheat(size_t index) {
    if (index >= cheats_.size()) {
        return false;
    }
    cheats_.erase(cheats_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

void CheatEngine::clearCheats() {
    cheats_.clear();
}

bool CheatEngine::loadFromFile(const std::filesystem::path& path,
                               std::string* errorText,
                               size_t* loadedCount) {
    std::vector<CheatEntry> loaded;
    if (!loadCheatFile(path, loaded, errorText)) {
        return false;
    }

    std::vector<CheatEntry> validated;
    validated.reserve(loaded.size());
    for (const CheatEntry& cheat : loaded) {
        std::string validateError;
        if (!validateCheatEntry(cheat, &validateError)) {
            if (errorText) {
                *errorText = validateError.empty() ? "Cheat file contains invalid entries" : validateError;
            }
            return false;
        }
        validated.push_back(cheat);
    }

    cheats_ = std::move(validated);
    if (loadedCount) {
        *loadedCount = cheats_.size();
    }
    return true;
}

bool CheatEngine::saveToFile(const std::filesystem::path& path, std::string* errorText) const {
    return saveCheatFile(path, cheats_, errorText);
}

void CheatEngine::applyEnabledCheats() {
    for (const CheatEntry& cheat : cheats_) {
        if (!cheat.enabled) {
            continue;
        }
        if (cheat.mode == CheatMode::Freeze) {
            writeTypedValue(memory_, cheat.address, cheat.type, cheat.value);
        }
    }
}

bool CheatEngine::readValue(u32 address, CheatValueType type, u32& outValue) const {
    if (!isAddressWritableForType(address, type)) {
        return false;
    }
    return readTypedValue(memory_, address, type, outValue);
}

bool CheatEngine::writeValueOnce(u32 address, CheatValueType type, u32 value) {
    if (!isAddressWritableForType(address, type)) {
        return false;
    }
    return writeTypedValue(memory_, address, type, value);
}

bool CheatEngine::freezeSearchResult(size_t index, u32 value, const std::string& name, std::string* errorText) {
    SearchCandidate candidate;
    if (!ramSearch_.getResult(index, candidate)) {
        if (errorText) *errorText = "Search result not found";
        return false;
    }

    CheatEntry cheat;
    cheat.address = candidate.where.address;
    cheat.value = value;
    cheat.type = ramSearch_.valueType();
    cheat.mode = CheatMode::Freeze;
    cheat.enabled = true;
    cheat.code = formatRawCheatCode(cheat.address, value, cheat.type);
    cheat.name = name.empty() ? defaultSearchCheatName(cheat.address, cheat.type) : name;
    return addOrUpdateCheat(cheat, errorText);
}

bool CheatEngine::setSearchResultValueOnce(size_t index, u32 value) {
    SearchAddress address;
    if (!ramSearch_.getResultAddress(index, address)) {
        return false;
    }
    return writeValueOnce(address.address, ramSearch_.valueType(), value);
}

bool CheatEngine::readSearchResultCurrentValue(size_t index, u32& outValue) const {
    SearchAddress address;
    if (!ramSearch_.getResultAddress(index, address)) {
        return false;
    }
    return readValue(address.address, ramSearch_.valueType(), outValue);
}

bool CheatEngine::isAddressWritableForType(u32 address, CheatValueType type) const {
    std::vector<MemoryRange> ranges;
    memory_.enumerateWritableRanges(ranges);
    for (const MemoryRange& range : ranges) {
        if (addressFitsRange(address, type, range)) {
            return true;
        }
    }
    return false;
}

bool CheatEngine::validateCheatEntry(const CheatEntry& cheat, std::string* errorText) const {
    if (errorText) errorText->clear();
    if (!isAddressWritableForType(cheat.address, cheat.type)) {
        if (errorText) *errorText = "Address is not in writable memory";
        return false;
    }
    if ((cheat.type == CheatValueType::U16 || cheat.type == CheatValueType::S16 ||
         cheat.type == CheatValueType::U32 || cheat.type == CheatValueType::S32) &&
        (cheat.address & 1u)) {
        if (errorText) *errorText = "16-bit and 32-bit cheats require even address";
        return false;
    }
    return true;
}

} // namespace Cheats
