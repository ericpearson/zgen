// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "memory/cartridge.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
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

static void writeBE16(std::vector<u8>& rom, size_t offset, u16 value) {
    rom[offset] = static_cast<u8>((value >> 8) & 0xFF);
    rom[offset + 1] = static_cast<u8>(value & 0xFF);
}

static void writeBE32(std::vector<u8>& rom, size_t offset, u32 value) {
    rom[offset] = static_cast<u8>((value >> 24) & 0xFF);
    rom[offset + 1] = static_cast<u8>((value >> 16) & 0xFF);
    rom[offset + 2] = static_cast<u8>((value >> 8) & 0xFF);
    rom[offset + 3] = static_cast<u8>(value & 0xFF);
}

static void writePaddedField(std::vector<u8>& rom, size_t offset, size_t len, const char* text) {
    std::memset(rom.data() + offset, ' ', len);
    const size_t textLen = std::strlen(text);
    const size_t copyLen = textLen < len ? textLen : len;
    std::memcpy(rom.data() + offset, text, copyLen);
}

static std::vector<u8> makeRom(const char* name, const char* region, bool withSram) {
    std::vector<u8> rom(0x400, 0);

    writeBE32(rom, 0x000, 0x00FF0000);
    writeBE32(rom, 0x004, 0x00000200);

    writePaddedField(rom, 0x100, 16, "SEGA GENESIS");
    writePaddedField(rom, 0x120, 48, name);
    writePaddedField(rom, 0x150, 48, name);
    writePaddedField(rom, 0x180, 14, "GM TEST-00");
    writeBE16(rom, 0x18E, 0x0000);
    writePaddedField(rom, 0x190, 16, "J               ");
    writeBE32(rom, 0x1A0, 0x00000000);
    writeBE32(rom, 0x1A4, static_cast<u32>(rom.size() - 1));

    if (withSram) {
        writePaddedField(rom, 0x1B0, 12, "RA");
        writeBE32(rom, 0x1B4, 0x00200000);
        writeBE32(rom, 0x1B8, 0x002000FF);
    } else {
        writePaddedField(rom, 0x1B0, 12, "  ");
    }

    writePaddedField(rom, 0x1F0, 3, region);
    return rom;
}

static bool writeRomFile(const std::filesystem::path& path, const std::vector<u8>& rom) {
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) {
        return false;
    }
    const size_t written = std::fwrite(rom.data(), 1, rom.size(), f);
    std::fclose(f);
    return written == rom.size();
}

static void testReloadClearsCartridgeState() {
    beginGroup("reload clears cartridge state");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_cartridge_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path rom1Path = dir / "with_sram.bin";
    const std::filesystem::path rom2Path = dir / "without_sram.bin";

    check(writeRomFile(rom1Path, makeRom("FIRST GAME", "U", true)), "writes first ROM");
    check(writeRomFile(rom2Path, makeRom("SECOND GAME", "J", false)), "writes second ROM");

    Cartridge cartridge;
    check(cartridge.load(rom1Path.string().c_str()), "loads first ROM");
    check(cartridge.hasSRAM(), "first ROM enables SRAM");
    check(cartridge.getGameName() == "FIRST GAME", "first ROM name parsed");

    check(cartridge.load(rom2Path.string().c_str()), "loads second ROM");
    check(!cartridge.hasSRAM(), "second ROM does not inherit SRAM");
    check(cartridge.getGameName() == "SECOND GAME", "second ROM name replaces first");

    const ROMHeader& header = cartridge.getHeader();
    check(header.region[0] == 'J', "second ROM region replaces first");

    std::error_code ec;
    std::filesystem::remove(rom1Path, ec);
    std::filesystem::remove(rom2Path, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

int main() {
    std::printf("Running cartridge tests...\n");

    testReloadClearsCartridgeState();

    std::printf("\nResults: %d/%d passed", passedTests, totalTests);
    if (failedTests == 0) {
        std::printf(" - ALL TESTS PASSED\n");
        return 0;
    }

    std::printf(" - %d FAILED\n", failedTests);
    return 1;
}
