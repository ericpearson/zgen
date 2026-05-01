// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
#include "cheats/cheat_engine.h"

#include <filesystem>

class GenesisCheatMemoryBridge final : public Cheats::CheatMemoryInterface {
public:
    explicit GenesisCheatMemoryBridge(Genesis& owner)
        : owner_(owner) {
    }

    bool read8(u32 address, u8& out) const override { return owner_.cheatRead8(address, out); }
    bool read16(u32 address, u16& out) const override { return owner_.cheatRead16(address, out); }
    bool write8(u32 address, u8 value) override { return owner_.cheatWrite8(address, value); }
    bool write16(u32 address, u16 value) override { return owner_.cheatWrite16(address, value); }
    void enumerateWritableRanges(std::vector<Cheats::MemoryRange>& out) const override {
        owner_.cheatEnumerateWritableRanges(out);
    }

private:
    Genesis& owner_;
};

Genesis::~Genesis() {
}

void Genesis::initCheatEngine() {
    cheatMemory_ = std::make_unique<GenesisCheatMemoryBridge>(*this);
    cheatEngine_ = std::make_unique<Cheats::CheatEngine>(*cheatMemory_);
}

bool Genesis::addCheat(const std::string& code, const std::string& name, std::string* errorText) {
    return cheatEngine_->addRawCheat(code, name, errorText);
}

bool Genesis::toggleCheat(size_t index) {
    return cheatEngine_->toggleCheat(index);
}

bool Genesis::removeCheat(size_t index) {
    return cheatEngine_->removeCheat(index);
}

void Genesis::clearCheats() {
    cheatEngine_->clearCheats();
}

const std::vector<Genesis::CheatEntry>& Genesis::getCheats() const {
    return cheatEngine_->getCheats();
}

bool Genesis::loadCheatsFromFile(std::string* errorText, size_t* loadedCount) {
    return cheatEngine_->loadFromFile(getCheatPath(), errorText, loadedCount);
}

bool Genesis::saveCheatsToFile(std::string* errorText) const {
    return cheatEngine_->saveToFile(getCheatPath(), errorText);
}

std::string Genesis::getCheatPath() const {
    if (currentRomPath_.empty()) {
        return {};
    }

    std::filesystem::path cheatPath(currentRomPath_);
    cheatPath.replace_extension(".cht");
    return cheatPath.string();
}

void Genesis::resetRamSearch() {
    cheatEngine_->ramSearch().reset();
}

bool Genesis::startRamSearchKnownValue(CheatValueType type, s32 targetValue, SearchHeuristic heuristic) {
    return cheatEngine_->ramSearch().startKnownValue(*cheatMemory_, type, targetValue, heuristic);
}

bool Genesis::startRamSearchUnknown(CheatValueType type) {
    return cheatEngine_->ramSearch().startUnknown(*cheatMemory_, type);
}

bool Genesis::refineRamSearch(SearchCompareMode mode,
                              std::optional<s32> compareValue,
                              SearchHeuristic heuristic) {
    return cheatEngine_->ramSearch().refine(*cheatMemory_, mode, compareValue, heuristic);
}

size_t Genesis::getRamSearchResultCount() const {
    return cheatEngine_->ramSearch().resultCount();
}

void Genesis::getRamSearchResults(size_t offset,
                                  size_t limit,
                                  std::vector<SearchCandidate>& out) const {
    cheatEngine_->ramSearch().getResults(offset, limit, out);
}

bool Genesis::getRamSearchResult(size_t index, SearchCandidate& out) const {
    return cheatEngine_->ramSearch().getResult(index, out);
}

bool Genesis::setRamSearchResultValueOnce(size_t index, u32 value) {
    return cheatEngine_->setSearchResultValueOnce(index, value);
}

bool Genesis::freezeRamSearchResult(size_t index,
                                    u32 value,
                                    const std::string& name,
                                    std::string* errorText) {
    return cheatEngine_->freezeSearchResult(index, value, name, errorText);
}

bool Genesis::refreshRamSearchResultValue(size_t index, u32& outValue) const {
    return cheatEngine_->readSearchResultCurrentValue(index, outValue);
}

bool Genesis::isRamSearchActive() const {
    return cheatEngine_->ramSearch().active();
}

Genesis::CheatValueType Genesis::getRamSearchValueType() const {
    return cheatEngine_->ramSearch().valueType();
}

bool Genesis::cheatRead8(u32 address, u8& outValue) const {
    if ((address & 0xFF0000u) == 0xFF0000u) {
        outValue = const_cast<Bus&>(bus).read8(address);
        return true;
    }
    if (address >= 0xA00000u && address <= 0xA01FFFu) {
        outValue = const_cast<Bus&>(bus).read8(address);
        return true;
    }
    if (bus.sramMapped && cartridge.hasSRAM()) {
        const ROMHeader& header = cartridge.getHeader();
        if (header.ramEnd >= header.ramStart &&
            address >= header.ramStart &&
            address <= header.ramEnd) {
            outValue = const_cast<Bus&>(bus).read8(address);
            return true;
        }
    }
    return false;
}

bool Genesis::cheatRead16(u32 address, u16& outValue) const {
    if ((address & 1u) != 0) {
        return false;
    }
    if ((address & 0xFF0000u) == 0xFF0000u) {
        outValue = const_cast<Bus&>(bus).read16(address);
        return true;
    }
    if (address >= 0xA00000u && address <= 0xA01FFEu) {
        outValue = const_cast<Bus&>(bus).read16(address);
        return true;
    }
    if (bus.sramMapped && cartridge.hasSRAM()) {
        const ROMHeader& header = cartridge.getHeader();
        if (header.ramEnd >= header.ramStart + 1 &&
            address >= header.ramStart &&
            address + 1 <= header.ramEnd) {
            outValue = const_cast<Bus&>(bus).read16(address);
            return true;
        }
    }
    return false;
}

bool Genesis::cheatWrite8(u32 address, u8 value) {
    if ((address & 0xFF0000u) == 0xFF0000u) {
        bus.write8(address, value);
        return true;
    }
    if (address >= 0xA00000u && address <= 0xA01FFFu) {
        bus.write8(address, value);
        return true;
    }
    if (bus.sramMapped && cartridge.hasSRAM()) {
        const ROMHeader& header = cartridge.getHeader();
        if (header.ramEnd >= header.ramStart &&
            address >= header.ramStart &&
            address <= header.ramEnd) {
            bus.write8(address, value);
            return true;
        }
    }
    return false;
}

bool Genesis::cheatWrite16(u32 address, u16 value) {
    if ((address & 1u) != 0) {
        return false;
    }
    if ((address & 0xFF0000u) == 0xFF0000u) {
        bus.write16(address, value);
        return true;
    }
    if (address >= 0xA00000u && address <= 0xA01FFEu) {
        bus.write16(address, value);
        return true;
    }
    if (bus.sramMapped && cartridge.hasSRAM()) {
        const ROMHeader& header = cartridge.getHeader();
        if (header.ramEnd >= header.ramStart + 1 &&
            address >= header.ramStart &&
            address + 1 <= header.ramEnd) {
            bus.write16(address, value);
            return true;
        }
    }
    return false;
}

void Genesis::cheatEnumerateWritableRanges(std::vector<Cheats::MemoryRange>& out) const {
    out.clear();
    out.push_back({0xFF0000u, 0xFFFFFFu, Cheats::SearchRegionKind::MainRam});
    out.push_back({0xA00000u, 0xA01FFFu, Cheats::SearchRegionKind::Z80Ram});
    if (bus.sramMapped && cartridge.hasSRAM()) {
        const ROMHeader& header = cartridge.getHeader();
        if (header.ramEnd >= header.ramStart) {
            out.push_back({header.ramStart, header.ramEnd, Cheats::SearchRegionKind::SRAM});
        }
    }
}
