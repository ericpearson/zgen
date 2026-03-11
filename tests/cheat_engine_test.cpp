// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cheats/cheat_engine.h"

#include <filesystem>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

static int totalTests = 0;
static int passedTests = 0;
static int failedTests = 0;
static int groupTests = 0;
static int groupPassed = 0;
static const char* currentGroup = nullptr;

static void beginGroup(const char* name) {
    currentGroup = name;
    groupTests = 0;
    groupPassed = 0;
    std::printf("  Testing %s...", name);
    std::fflush(stdout);
}

static void endGroup() {
    if (groupTests == groupPassed) {
        std::printf(" OK (%d tests)\n", groupTests);
    } else {
        std::printf(" FAILED (%d/%d passed)\n", groupPassed, groupTests);
    }
}

static bool check(bool condition, const char* desc) {
    totalTests++;
    groupTests++;
    if (condition) {
        passedTests++;
        groupPassed++;
        return true;
    }

    failedTests++;
    std::printf("\n    FAIL: %s [%s]", desc, currentGroup);
    return false;
}

class FakeCheatMemory final : public Cheats::CheatMemoryInterface {
public:
    bool enableSRAM = true;
    std::array<u8, 0x10000> mainRam{};
    std::array<u8, 0x2000> z80Ram{};
    std::array<u8, 0x0100> sram{};

    bool read8(u32 address, u8& out) const override {
        if ((address & 0xFF0000u) == 0xFF0000u) {
            out = mainRam[address & 0xFFFFu];
            return true;
        }
        if (address >= 0xA00000u && address <= 0xA01FFFu) {
            out = z80Ram[address & 0x1FFFu];
            return true;
        }
        if (enableSRAM && address >= 0x200000u && address <= 0x2000FFu) {
            out = sram[address - 0x200000u];
            return true;
        }
        return false;
    }

    bool read16(u32 address, u16& out) const override {
        u8 hi = 0;
        u8 lo = 0;
        if (!read8(address, hi) || !read8(address + 1, lo)) {
            return false;
        }
        out = static_cast<u16>((static_cast<u16>(hi) << 8) | lo);
        return true;
    }

    bool write8(u32 address, u8 value) override {
        if ((address & 0xFF0000u) == 0xFF0000u) {
            mainRam[address & 0xFFFFu] = value;
            return true;
        }
        if (address >= 0xA00000u && address <= 0xA01FFFu) {
            z80Ram[address & 0x1FFFu] = value;
            return true;
        }
        if (enableSRAM && address >= 0x200000u && address <= 0x2000FFu) {
            sram[address - 0x200000u] = value;
            return true;
        }
        return false;
    }

    bool write16(u32 address, u16 value) override {
        return write8(address, static_cast<u8>((value >> 8) & 0xFFu)) &&
               write8(address + 1, static_cast<u8>(value & 0xFFu));
    }

    void enumerateWritableRanges(std::vector<Cheats::MemoryRange>& out) const override {
        out.clear();
        out.push_back({0xFF0000u, 0xFFFFFFu, Cheats::SearchRegionKind::MainRam});
        out.push_back({0xA00000u, 0xA01FFFu, Cheats::SearchRegionKind::Z80Ram});
        if (enableSRAM) {
            out.push_back({0x200000u, 0x2000FFu, Cheats::SearchRegionKind::SRAM});
        }
    }
};

static void testCheatFormat() {
    beginGroup("raw cheat parsing");

    Cheats::CheatEntry entry;
    std::string error;
    check(Cheats::parseRawCheatCode("FF0000:12", entry, &error), "parses 8-bit raw code");
    check(entry.type == Cheats::CheatValueType::U8, "8-bit code infers U8");
    check(entry.code == "FF0000:12", "8-bit code normalized");

    check(Cheats::parseRawCheatCode("FF0000:1234", entry, &error), "parses 16-bit raw code");
    check(entry.type == Cheats::CheatValueType::U16, "16-bit code infers U16");
    check(entry.code == "FF0000:1234", "16-bit code normalized");

    check(Cheats::parseRawCheatCode("FF0000:12345678", entry, &error), "parses 32-bit raw code");
    check(entry.type == Cheats::CheatValueType::U32, "32-bit code infers U32");
    check(entry.code == "FF0000:12345678", "32-bit code normalized");

    check(!Cheats::parseRawCheatCode("FF0001:1234", entry, &error), "rejects odd-aligned 16-bit code");

    endGroup();
}

static void testSearchEnumeration() {
    beginGroup("search address enumeration");

    std::vector<Cheats::MemoryRange> ranges = {
        {0xFF0000u, 0xFF0003u, Cheats::SearchRegionKind::MainRam},
        {0xA00000u, 0xA00003u, Cheats::SearchRegionKind::Z80Ram},
    };
    std::vector<Cheats::SearchAddress> addresses;

    Cheats::enumerateSearchAddresses(ranges, Cheats::CheatValueType::U8, addresses);
    check(addresses.size() == 8, "u8 search walks every byte");

    Cheats::enumerateSearchAddresses(ranges, Cheats::CheatValueType::U16, addresses);
    check(addresses.size() == 4, "u16 search walks even-aligned words");

    Cheats::enumerateSearchAddresses(ranges, Cheats::CheatValueType::U32, addresses);
    check(addresses.size() == 2, "u32 search only keeps in-range even-aligned values");

    endGroup();
}

static void testKnownValueAndRefine() {
    beginGroup("known value and refine");

    FakeCheatMemory memory;
    memory.write16(0xFF0000u, 100);
    memory.write16(0xFF0002u, 50);
    memory.write8(0xFF0010u, 1);
    memory.write8(0xFF0011u, 0);

    Cheats::RamSearchSession session;
    check(session.startKnownValue(memory, Cheats::CheatValueType::U8, 1, Cheats::SearchHeuristic::BooleanLike),
          "known-value search starts");
    check(session.resultCount() >= 2, "boolean heuristic keeps 0/1 candidates");

    session.reset();
    check(session.startUnknown(memory, Cheats::CheatValueType::U16), "unknown snapshot starts");
    memory.write16(0xFF0000u, 101);
    memory.write16(0xFF0002u, 49);
    check(session.refine(memory, Cheats::SearchCompareMode::Increased, std::nullopt, Cheats::SearchHeuristic::Off),
          "refine by increase succeeds");
    check(session.resultCount() == 1, "refine narrows to one candidate");

    Cheats::SearchCandidate candidate;
    check(session.getResult(0, candidate), "single result is readable");
    check(candidate.where.address == 0xFF0000u, "increased candidate address is correct");
    check(candidate.currentValue == 101, "current value updates after refine");

    endGroup();
}

static void testEngineFreezeAndSet() {
    beginGroup("engine freeze and set once");

    FakeCheatMemory memory;
    memory.write16(0xFF0000u, 7);

    Cheats::CheatEngine engine(memory);
    check(engine.ramSearch().startKnownValue(memory, Cheats::CheatValueType::U16, 7, Cheats::SearchHeuristic::Off),
          "search locates initial value");
    check(engine.ramSearch().resultCount() == 1, "search finds one exact candidate");

    check(engine.setSearchResultValueOnce(0, 9), "set once writes memory");
    u16 word = 0;
    check(memory.read16(0xFF0000u, word) && word == 9, "memory changed after set once");

    std::string error;
    check(engine.freezeSearchResult(0, 11, "Lives", &error), "freeze creates cheat from search result");
    check(engine.getCheats().size() == 1, "freeze adds one cheat");
    check(engine.getCheats()[0].name == "Lives", "custom freeze name is preserved");

    memory.write16(0xFF0000u, 3);
    engine.applyEnabledCheats();
    check(memory.read16(0xFF0000u, word) && word == 11, "freeze cheat reapplies target value");

    endGroup();
}

static void testCheatFileRoundTrip() {
    beginGroup("cheat file round trip");

    FakeCheatMemory memory;
    Cheats::CheatEngine engine(memory);
    std::string error;

    check(engine.addRawCheat("FF0000:12", "Lives", &error), "adds raw cheat before save");
    check(engine.addRawCheat("FF0002:1234", "Score\tBoost", &error), "adds second raw cheat before save");
    check(engine.toggleCheat(1), "can disable second cheat before save");

    const auto path = std::filesystem::temp_directory_path() / "genesis_cheat_engine_test.cht";
    std::filesystem::remove(path);

    check(engine.saveToFile(path, &error), "saves cheat sidecar file");

    Cheats::CheatEngine loaded(memory);
    size_t loadedCount = 0;
    check(loaded.loadFromFile(path, &error, &loadedCount), "loads cheat sidecar file");
    check(loadedCount == 2, "round trip preserves cheat count");
    check(loaded.getCheats().size() == 2, "loaded engine has both cheats");
    check(loaded.getCheats()[0].name == "Lives", "loaded first name matches");
    check(loaded.getCheats()[1].name == "Score\tBoost", "loaded escaped name matches");
    check(!loaded.getCheats()[1].enabled, "loaded enabled state matches");

    std::filesystem::remove(path);
    endGroup();
}

int main() {
    std::printf("Running cheat engine tests...\n");

    testCheatFormat();
    testSearchEnumeration();
    testKnownValueAndRefine();
    testEngineFreezeAndSet();
    testCheatFileRoundTrip();

    std::printf("\nResults: %d/%d passed", passedTests, totalTests);
    if (failedTests == 0) {
        std::printf(" OK\n");
        return 0;
    }

    std::printf(" FAILED (%d failed)\n", failedTests);
    return 1;
}
