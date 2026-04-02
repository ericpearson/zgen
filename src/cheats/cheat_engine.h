// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "cheats/cheat_file.h"
#include "cheats/cheat_format.h"
#include "cheats/ram_search.h"
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Cheats {

class CheatEngine {
public:
    explicit CheatEngine(CheatMemoryInterface& memory);

    bool addRawCheat(const std::string& code, const std::string& name, std::string* errorText = nullptr);
    bool addOrUpdateCheat(const CheatEntry& cheat, std::string* errorText = nullptr);
    bool toggleCheat(size_t index);
    bool removeCheat(size_t index);
    void clearCheats();
    bool loadFromFile(const std::filesystem::path& path,
                      std::string* errorText = nullptr,
                      size_t* loadedCount = nullptr);
    bool saveToFile(const std::filesystem::path& path, std::string* errorText = nullptr) const;

    const std::vector<CheatEntry>& getCheats() const { return cheats_; }

    void applyEnabledCheats();

    bool readValue(u32 address, CheatValueType type, u32& outValue) const;
    bool writeValueOnce(u32 address, CheatValueType type, u32 value);

    RamSearchSession& ramSearch() { return ramSearch_; }
    const RamSearchSession& ramSearch() const { return ramSearch_; }

    bool freezeSearchResult(size_t index, u32 value, const std::string& name, std::string* errorText = nullptr);
    bool setSearchResultValueOnce(size_t index, u32 value);
    bool readSearchResultCurrentValue(size_t index, u32& outValue) const;

private:
    bool isAddressWritableForType(u32 address, CheatValueType type) const;
    bool validateCheatEntry(const CheatEntry& cheat, std::string* errorText) const;

    CheatMemoryInterface& memory_;
    std::vector<CheatEntry> cheats_;
    RamSearchSession ramSearch_;
};

} // namespace Cheats
