// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// Bus timing test suite
// Tests Z80 bank access timing and 68K Z80-area wait states

#include "memory/bus.h"
#include "memory/cartridge.h"
#include "genesis.h"
#include "cpu/m68k.h"
#include "cpu/z80.h"
#include "video/vdp.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

// --- Test framework ---
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
    printf("  Testing %s...", name);
    fflush(stdout);
}

static void endGroup() {
    if (groupTests == groupPassed) {
        printf(" OK (%d tests)\n", groupTests);
    } else {
        printf(" FAILED (%d/%d passed)\n", groupPassed, groupTests);
    }
}

static bool check(bool condition, const char* desc) {
    totalTests++;
    groupTests++;
    if (condition) {
        passedTests++;
        groupPassed++;
        return true;
    } else {
        failedTests++;
        printf("\n    FAIL: %s [%s]", desc, currentGroup);
        return false;
    }
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

static std::vector<u8> makeOverlappingSRAMRom() {
    std::vector<u8> rom(0x204000, 0);

    writeBE32(rom, 0x000, 0x00FF0000);
    writeBE32(rom, 0x004, 0x00000200);
    writePaddedField(rom, 0x100, 16, "SEGA GENESIS");
    writePaddedField(rom, 0x120, 48, "SRAM OVERLAP TEST");
    writePaddedField(rom, 0x150, 48, "SRAM OVERLAP TEST");
    writePaddedField(rom, 0x180, 14, "GM TEST-00");
    writePaddedField(rom, 0x190, 16, "J               ");
    writeBE32(rom, 0x1A0, 0x00000000);
    writeBE32(rom, 0x1A4, static_cast<u32>(rom.size() - 1));
    writePaddedField(rom, 0x1B0, 12, "RA");
    rom[0x1B2] = 0xF8; // Odd-byte SRAM bus.
    rom[0x1B3] = 0x20;
    writeBE32(rom, 0x1B4, 0x00200001);
    writeBE32(rom, 0x1B8, 0x00200003);
    writePaddedField(rom, 0x1F0, 3, "U");

    rom[0x200000] = 0x11;
    rom[0x200001] = 0x22;
    rom[0x200002] = 0x33;
    rom[0x200003] = 0x44;
    writeBE16(rom, 0x18E, 0x4466);
    return rom;
}

static std::vector<u8> makeDirectSRAMAtRomEndRom() {
    std::vector<u8> rom(0x200000, 0);

    writeBE32(rom, 0x000, 0x00FF0000);
    writeBE32(rom, 0x004, 0x00000200);
    writePaddedField(rom, 0x100, 16, "SEGA GENESIS");
    writePaddedField(rom, 0x120, 48, "DIRECT SRAM TEST");
    writePaddedField(rom, 0x150, 48, "DIRECT SRAM TEST");
    writePaddedField(rom, 0x180, 14, "GM TEST-01");
    writePaddedField(rom, 0x190, 16, "J               ");
    writeBE32(rom, 0x1A0, 0x00000000);
    writeBE32(rom, 0x1A4, static_cast<u32>(rom.size() - 1));
    writePaddedField(rom, 0x1B0, 12, "RA");
    rom[0x1B2] = 0xF8; // Odd-byte SRAM bus.
    rom[0x1B3] = 0x20;
    writeBE32(rom, 0x1B4, 0x00200001);
    writeBE32(rom, 0x1B8, 0x00203FFF);
    writePaddedField(rom, 0x1F0, 3, "U");

    rom[0x1FFFFE] = 0xCA;
    rom[0x1FFFFF] = 0xFE;
    writeBE16(rom, 0x18E, 0xCAFE);
    return rom;
}

static std::vector<u8> makeZ80BankStallSchedulerRom() {
    std::vector<u8> rom(0x4000, 0);

    writeBE32(rom, 0x000, 0x00FF0000);
    writeBE32(rom, 0x004, 0x00000200);
    writePaddedField(rom, 0x100, 16, "SEGA GENESIS");
    writePaddedField(rom, 0x120, 48, "Z80 BANK STALL TEST");
    writePaddedField(rom, 0x150, 48, "Z80 BANK STALL TEST");
    writePaddedField(rom, 0x180, 14, "GM TEST-02");
    writePaddedField(rom, 0x190, 16, "J               ");
    writeBE32(rom, 0x1A0, 0x00000000);
    writeBE32(rom, 0x1A4, static_cast<u32>(rom.size() - 1));
    writePaddedField(rom, 0x1F0, 3, "U");

    // Keep the 68K busy in a tight loop while the Z80 test program runs.
    rom[0x200] = 0x60; // BRA.S -2
    rom[0x201] = 0xFE;
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

// --- BusTest: friend class for direct state inspection ---
class BusTest {
public:
    static void initMinimal(Bus& bus) {
        // Initialize bus with minimal state for testing
        bus.reset();
        bus.z80Bank = 0;  // Bank points to ROM address 0
    }

    static int getZ80BankAccessCycles(Bus& bus) {
        return bus.z80BankAccessCycles;
    }

    static int getPendingM68KZ80BusStallCycles(Bus& bus) {
        return bus.pendingM68KZ80BusStallCycles;
    }

    static int consumeM68KZ80BusStallCycles(Bus& bus, int maxCycles) {
        return bus.consumeM68KZ80BusStallCycles(maxCycles);
    }

    static void resetZ80BankAccessCycles(Bus& bus) {
        bus.z80BankAccessCycles = 0;
        bus.pendingM68KZ80BusStallCycles = 0;
    }

    static int getM68KZ80AreaWaitCycles(Bus& bus) {
        return bus.m68kZ80AreaWaitCycles;
    }

    static void resetM68KZ80AreaWaitCycles(Bus& bus) {
        bus.m68kZ80AreaWaitCycles = 0;
    }

    // Set up for testing without full Genesis integration
    static void setZ80BusRequested(Bus& bus, bool req) {
        bus.z80BusRequested = req;
    }

    static void setZ80Reset(Bus& bus, bool reset) {
        bus.z80Reset = reset;
    }

    // Reset z80CycleCounter for testing
    static void resetZ80CycleCounter(Bus& bus) {
        bus.z80CycleCounter = 0;
        bus.busreqAssertCycle = 0;
        bus.busreqDeassertCycle = 0;
        bus.busreqAssertMclk = 0;
        bus.busreqDeassertMclk = 0;
    }

    static u8* getZ80Ram(Bus& bus) {
        return bus.z80Ram;
    }

    // BUSREQ/BUSACK timing accessors
    static bool getZ80BusAck(Bus& bus) {
        return bus.z80BusAck;
    }

    static int getBusreqAssertCycle(Bus& bus) {
        return bus.busreqAssertCycle;
    }

    static int getBusreqDeassertCycle(Bus& bus) {
        return bus.busreqDeassertCycle;
    }

    static void setMasterCycle(Bus& bus, int cycle) {
        bus.masterCycle = cycle;
    }

    static int getMasterCycle(Bus& bus) {
        return bus.masterCycle;
    }

    // Simulate writing to BUSREQ register (0xA11100)
    static void writeBusreq(Bus& bus, bool request) {
        bus.write16(0xA11100, request ? 0x0100 : 0x0000);
    }

    // Update BUSACK state based on elapsed Z80 cycles
    static void updateBusAck(Bus& bus, int z80Cycles) {
        bus.updateBusAck(z80Cycles);
    }
};

class GenesisTimingTest {
public:
    static void resetZ80AudioTrace(Genesis& genesis) {
        genesis.z80AudioTrace_ = {};
        genesis.ymPendingCycles_ = 0;
        genesis.ymBurstTotalCycles_ = 0;
        genesis.ymBurstDrained_ = 0;
        genesis.z80BurstInitialDebt_ = 0;
    }

    static void clockZ80AndYM(Genesis& genesis, int m68kCycles) {
        genesis.clockZ80AndYM(m68kCycles);
    }

    static void clockZ80Master(Genesis& genesis, int masterCycles) {
        genesis.clockZ80Master(masterCycles);
    }

    static void clockYM(Genesis& genesis, int m68kCycles) {
        genesis.clockYM(m68kCycles);
    }

    static void assertZ80InterruptPulse(Genesis& genesis) {
        genesis.assertZ80InterruptPulse();
    }

    static int getZ80InterruptPulseCycles(const Genesis& genesis) {
        return genesis.z80InterruptPulseCycles_;
    }

    static u64 getZ80InterruptPulseStartMasterCycle(const Genesis& genesis) {
        return genesis.z80InterruptPulseStartMasterCycle_;
    }

    static u64 getZ80InterruptPulseEndMasterCycle(const Genesis& genesis) {
        return genesis.z80InterruptPulseEndMasterCycle_;
    }

    static bool getZ80InterruptLine(const Genesis& genesis) {
        return genesis.z80.state.irqLine;
    }

    static u64 getZ80InterruptPulseExpirations(const Genesis& genesis) {
        return genesis.z80AudioTrace_.z80InterruptPulseExpirations;
    }

    static int getZ80CycleAccum(const Genesis& genesis) {
        return genesis.z80CycleAccum;
    }

    static u64 getZ80CyclesBudgeted(const Genesis& genesis) {
        return genesis.z80AudioTrace_.z80CyclesBudgeted;
    }

    static u64 getZ80CyclesHalted(const Genesis& genesis) {
        return genesis.z80AudioTrace_.z80CyclesHalted;
    }

    static void haltZ80(Genesis& genesis) {
        genesis.bus.z80Reset = true;
        genesis.bus.z80BusRequested = false;
    }

    static u64 getYMWrites(const Genesis& genesis) {
        return genesis.z80AudioTrace_.ymWrites;
    }

    static u64 getYMSyncCycles(const Genesis& genesis) {
        return genesis.z80AudioTrace_.ymSyncCycles;
    }

    static u64 getMasterCycle(const Genesis& genesis) {
        return genesis.masterCycle_;
    }

    static u64 getScanlineStartMasterCycle(const Genesis& genesis) {
        return genesis.scanlineStartMasterCycle_;
    }

    static void resetMasterClock(Genesis& genesis) {
        genesis.resetMasterClock();
    }

    static void beginMasterClockScanline(Genesis& genesis) {
        genesis.beginMasterClockScanline();
    }

    static void syncMasterClockToM68KLineCycle(Genesis& genesis, int lineCycle) {
        genesis.syncMasterClockToM68KLineCycle(lineCycle);
    }

    static void endMasterClockScanline(Genesis& genesis) {
        genesis.endMasterClockScanline();
    }
};

struct ConnectedM68KBus {
    Bus bus;
    M68K cpu;

    ConnectedM68KBus() {
        BusTest::initMinimal(bus);
        cpu.connectBus(&bus);
        bus.connectCPU(&cpu);
        cpu.state.cycles = 0;
        BusTest::resetM68KZ80AreaWaitCycles(bus);
    }
};

struct ConnectedZ80BankBus {
    Bus bus;
    M68K m68k;
    Z80 z80;

    ConnectedZ80BankBus() {
        BusTest::initMinimal(bus);
        m68k.connectBus(&bus);
        z80.setBus(&bus);
        bus.connectCPU(&m68k);
        bus.connectZ80(&z80);
        BusTest::resetZ80BankAccessCycles(bus);
        z80.state.cycles = 0;
    }
};

struct ConnectedM68KVDPBus {
    Bus bus;
    M68K cpu;
    VDP vdp;

    ConnectedM68KVDPBus() {
        BusTest::initMinimal(bus);
        cpu.connectBus(&bus);
        bus.connectCPU(&cpu);
        bus.connectVDP(&vdp);
        vdp.connectBus(&bus);
        vdp.reset();
        vdp.setVideoStandard(VideoStandard::NTSC);
        cpu.state.cycles = 0;
    }
};

// =====================================================================
// Test 1: Z80 bank read adds timing penalty.
// Z80 bank reads add 3 * MCLKS_PER_Z80 (~18 master clocks), which equals
// 3 Z80 cycles.
// =====================================================================
static void testZ80BankReadTiming() {
    beginGroup("Z80 bank read timing");

    Bus bus;
    BusTest::initMinimal(bus);

    // Initial state: no accumulated cycles
    BusTest::resetZ80BankAccessCycles(bus);
    check(BusTest::getZ80BankAccessCycles(bus) == 0, "Initial cycles = 0");

    // Perform one bank read (address >= 0x8000)
    bus.z80Read(0x8000);
    check(BusTest::getZ80BankAccessCycles(bus) == 3, "One bank read adds 3 cycles");

    // Perform another bank read
    bus.z80Read(0x9000);
    check(BusTest::getZ80BankAccessCycles(bus) == 6, "Two bank reads add 6 cycles total");

    // Non-bank reads should not add cycles
    BusTest::resetZ80BankAccessCycles(bus);
    bus.z80Read(0x1000);  // Z80 RAM area
    check(BusTest::getZ80BankAccessCycles(bus) == 0, "Z80 RAM read adds no cycles");

    bus.z80Read(0x4000);  // YM2612 area
    check(BusTest::getZ80BankAccessCycles(bus) == 0, "YM2612 read adds no cycles");

    endGroup();
}

static void testZ80BankReadStallsM68K() {
    beginGroup("Z80 bank read stalls M68K");

    ConnectedZ80BankBus env;

    env.bus.z80Read(0x8000);
    check(BusTest::getZ80BankAccessCycles(env.bus) == 3, "Bank read charges 3 Z80 cycles");
    check(env.z80.state.cycles == 3, "Bank read adds the 3-cycle penalty to the executing Z80");
    check(BusTest::getPendingM68KZ80BusStallCycles(env.bus) == 8,
          "Bank read queues an 8-cycle M68K bus stall");

    int consumed = BusTest::consumeM68KZ80BusStallCycles(env.bus, 5);
    check(consumed == 5, "Partial M68K bus-stall consume returns requested cycles");
    check(BusTest::getPendingM68KZ80BusStallCycles(env.bus) == 3,
          "Partial M68K bus-stall consume leaves remaining cycles");

    consumed = BusTest::consumeM68KZ80BusStallCycles(env.bus, 10);
    check(consumed == 3, "Final M68K bus-stall consume returns remaining cycles");
    check(BusTest::getPendingM68KZ80BusStallCycles(env.bus) == 0,
          "Final M68K bus-stall consume clears pending cycles");

    endGroup();
}

// =====================================================================
// Test 2: Z80 bank write adds timing penalty
// Same 3 Z80 cycle penalty for writes
// =====================================================================
static void testZ80BankWriteTiming() {
    beginGroup("Z80 bank write timing");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::resetZ80BankAccessCycles(bus);

    // Write to bank area (0x8000+)
    bus.z80Write(0x8000, 0x42);
    check(BusTest::getZ80BankAccessCycles(bus) == 3, "One bank write adds 3 cycles");

    bus.z80Write(0xFFFF, 0x00);
    check(BusTest::getZ80BankAccessCycles(bus) == 6, "Two bank writes add 6 cycles total");

    // Non-bank writes should not add cycles
    BusTest::resetZ80BankAccessCycles(bus);
    bus.z80Write(0x1000, 0x00);  // Z80 RAM
    check(BusTest::getZ80BankAccessCycles(bus) == 0, "Z80 RAM write adds no cycles");

    endGroup();
}

static void testZ80BankWriteStallsM68K() {
    beginGroup("Z80 bank write stalls M68K");

    ConnectedZ80BankBus env;

    env.bus.z80Write(0x8000, 0x42);
    check(BusTest::getZ80BankAccessCycles(env.bus) == 3, "Bank write charges 3 Z80 cycles");
    check(env.z80.state.cycles == 3, "Bank write adds the 3-cycle penalty to the executing Z80");
    check(BusTest::getPendingM68KZ80BusStallCycles(env.bus) == 8,
          "Bank write queues an 8-cycle M68K bus stall");

    env.bus.z80Write(0x4000, 0x00);
    check(BusTest::getPendingM68KZ80BusStallCycles(env.bus) == 8,
          "Non-bank Z80 write does not queue an M68K bus stall");

    endGroup();
}

// =====================================================================
// Test 3: 68K Z80-area read adds wait state.
// 68K accesses to the Z80 area ($A00000-$A0FFFF) add 1 M68K cycle.
// =====================================================================
static void testM68KZ80AreaReadWait() {
    beginGroup("68K Z80-area read wait state");

    ConnectedM68KBus env;
    Bus& bus = env.bus;
    BusTest::setZ80BusRequested(bus, true);  // 68K has bus access

    // Initial state
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 0, "Initial wait cycles = 0");
    check(env.cpu.state.cycles == 0, "Initial M68K instruction cycles = 0");

    // Read from Z80 RAM area
    bus.read8(0xA00000);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 1, "Z80 RAM read adds 1 wait cycle");
    check(env.cpu.state.cycles == 1, "Z80 RAM read advances M68K instruction cycles");

    bus.read8(0xA01FFF);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 2, "Two Z80 RAM reads add 2 wait cycles");
    check(env.cpu.state.cycles == 2, "Two Z80 RAM reads advance M68K instruction cycles");

    // Read from YM2612 area (still in Z80 space)
    bus.read8(0xA04000);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 3, "YM2612 read adds 1 wait cycle");
    check(env.cpu.state.cycles == 3, "YM2612 read advances M68K instruction cycles");

    // Non-Z80-area reads should not add wait
    BusTest::resetM68KZ80AreaWaitCycles(bus);
    env.cpu.state.cycles = 0;
    bus.read8(0xFF0000);  // Main RAM
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 0, "Main RAM read adds no wait");
    check(env.cpu.state.cycles == 0, "Main RAM read does not advance M68K wait cycles");

    endGroup();
}

// =====================================================================
// Test 4: 68K Z80-area write adds wait state
// =====================================================================
static void testM68KZ80AreaWriteWait() {
    beginGroup("68K Z80-area write wait state");

    ConnectedM68KBus env;
    Bus& bus = env.bus;
    BusTest::setZ80BusRequested(bus, true);

    // Write to Z80 RAM
    bus.write8(0xA00000, 0x42);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 1, "Z80 RAM write adds 1 wait cycle");
    check(env.cpu.state.cycles == 1, "Z80 RAM write advances M68K instruction cycles");

    // Write to YM2612
    bus.write8(0xA04000, 0x00);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 2, "YM2612 write adds 1 wait cycle");
    check(env.cpu.state.cycles == 2, "YM2612 write advances M68K instruction cycles");

    endGroup();
}

// =====================================================================
// Test 5: 16-bit accesses to Z80 area
// =====================================================================
static void testM68KZ80Area16BitWait() {
    beginGroup("68K Z80-area 16-bit wait state");

    ConnectedM68KBus env;
    Bus& bus = env.bus;
    BusTest::setZ80BusRequested(bus, true);

    // 16-bit read from Z80 RAM
    bus.read16(0xA00000);
    // Treat word accesses as single bus transactions, so 16-bit = 1 wait.
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 1, "16-bit Z80 area read adds 1 wait cycle");
    check(env.cpu.state.cycles == 1, "16-bit Z80 area read advances M68K instruction cycles");

    // 16-bit write to Z80 RAM
    BusTest::resetM68KZ80AreaWaitCycles(bus);
    env.cpu.state.cycles = 0;
    bus.write16(0xA00000, 0x1234);
    check(BusTest::getM68KZ80AreaWaitCycles(bus) == 1, "16-bit Z80 area write adds 1 wait cycle");
    check(env.cpu.state.cycles == 1, "16-bit Z80 area write advances M68K instruction cycles");

    endGroup();
}

// =====================================================================
// Test 6: BUSREQ assertion delays BUSACK by ~3 Z80 cycles
// =====================================================================
// Bus::updateBusAck settles z80BusAck after the Z80 clock has advanced
// past the hardware threshold. Genesis::runZ80Burst calls updateBusAck
// with the number of Z80 cycles that ran (or the halted-cycle budget
// when the Z80 is stopped by BUSREQ). We advance that cycle counter
// directly here via BusTest::updateBusAck — matches production exactly.
static void testBusreqAckDelay() {
    beginGroup("BUSREQ->BUSACK delay");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::setZ80Reset(bus, false);  // Z80 running
    BusTest::resetZ80CycleCounter(bus);

    // Initially Z80 has the bus (BUSREQ not asserted)
    check(BusTest::getZ80BusAck(bus) == false, "Initial BUSACK = false (Z80 has bus)");

    // Assert BUSREQ at z80 cycle 0
    BusTest::writeBusreq(bus, true);

    // BUSACK must NOT be granted yet: no Z80 cycles have elapsed.
    check(BusTest::getZ80BusAck(bus) == false, "BUSACK not granted immediately");

    // Advance 2 Z80 cycles (below 3-cycle threshold)
    BusTest::updateBusAck(bus, 2);
    check(BusTest::getZ80BusAck(bus) == false, "BUSACK not granted after 2 Z80 cycles");

    // Advance 1 more — total 3, should now be granted
    BusTest::updateBusAck(bus, 1);
    check(BusTest::getZ80BusAck(bus) == true, "BUSACK granted after 3 Z80 cycles");

    endGroup();
}

// =====================================================================
// Test 7: BUSREQ deassert delays Z80 resume by ~1 Z80 cycle
// =====================================================================
static void testBusreqResumeDelay() {
    beginGroup("BUSREQ deassert resume delay");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::setZ80Reset(bus, false);  // Z80 running
    BusTest::resetZ80CycleCounter(bus);

    // Assert BUSREQ, let BUSACK settle.
    BusTest::writeBusreq(bus, true);
    BusTest::updateBusAck(bus, 5);  // well past the 3-cycle grant threshold
    check(BusTest::getZ80BusAck(bus) == true, "BUSACK granted (setup)");

    // Deassert BUSREQ — z80BusAck must stay true until updateBusAck sees
    // >= 1 Z80 cycle of post-deassert time.
    BusTest::writeBusreq(bus, false);
    check(BusTest::getZ80BusAck(bus) == true, "Z80 not resumed immediately");

    // 0 cycles after deassert still not enough (updateBusAck checks >=1).
    BusTest::updateBusAck(bus, 0);
    check(BusTest::getZ80BusAck(bus) == true, "Z80 not resumed after 0 cycles elapsed");

    // 1 Z80 cycle elapsed — Z80 resumes.
    BusTest::updateBusAck(bus, 1);
    check(BusTest::getZ80BusAck(bus) == false, "Z80 resumed after 1 Z80 cycle");

    endGroup();
}

// =====================================================================
// Test 8: 68K reads of BUSACK register reflect actual state
// Reading 0xA11100 word - bit 8 (D0 of high byte) = 0 when bus granted, 1 when Z80 running
// (Hardware convention: 0 = 68K can access, 1 = Z80 running)
// Note: On 68K, byte at 0xA11100 is high byte of word, so bit 0 of byte = bit 8 of word
// =====================================================================
static void testBusackRegisterRead() {
    beginGroup("BUSACK register read");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::setZ80Reset(bus, false);  // Z80 running
    BusTest::resetZ80CycleCounter(bus);

    // Initially Z80 has bus - bit 8 = 1 (Z80 running, bus not granted)
    u16 val = bus.read16(0xA11100);
    check((val & 0x0100) != 0, "BUSACK bit = 1 when Z80 has bus (not granted)");

    // Assert BUSREQ and let BUSACK settle (>= 3 Z80 cycles)
    BusTest::writeBusreq(bus, true);
    BusTest::updateBusAck(bus, 5);

    // Now 68K has bus - bit 8 = 0 (bus granted to 68K)
    val = bus.read16(0xA11100);
    check((val & 0x0100) == 0, "BUSACK bit = 0 when 68K has bus (granted)");

    endGroup();
}

static void testBusackRegisterReadWhileResetAsserted() {
    beginGroup("BUSACK register read while Z80 reset");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::setZ80Reset(bus, false);
    BusTest::resetZ80CycleCounter(bus);

    BusTest::writeBusreq(bus, true);
    BusTest::updateBusAck(bus, 5);
    check(BusTest::getZ80BusAck(bus) == true, "BUSACK granted before reset assertion");

    bus.write16(0xA11200, 0x0000); // Assert Z80 reset.
    u16 val = bus.read16(0xA11100);
    check((val & 0x0100) != 0,
          "BUSACK bit = 1 when Z80 reset is asserted");

    BusTest::writeBusreq(bus, false);
    val = bus.read16(0xA11100);
    check((val & 0x0100) != 0,
          "BUSACK bit = 1 when Z80 reset is asserted and BUSREQ released");

    endGroup();
}

// =====================================================================
// Test 8b: 68K BUSACK polling observes elapsed time inside one M68K burst
// =====================================================================
static void testBusackPollProgressesWithinM68KBurst() {
    beginGroup("BUSACK poll progresses within M68K burst");

    ConnectedM68KBus env;
    BusTest::setZ80Reset(env.bus, false);  // Z80 running
    BusTest::resetZ80CycleCounter(env.bus);
    env.cpu.totalCycles_ = 1000;
    env.cpu.state.cycles = 0;

    // Assert BUSREQ at the current 68K timestamp. A read before roughly
    // 3 Z80 cycles have elapsed should still report Z80 running.
    env.bus.write16(0xA11100, 0x0100, 0);
    u8 val = env.bus.read8(0xA11100, 2);
    check((val & 0x01) != 0, "BUSACK not granted before 3 Z80 cycles");

    // Poll again later in the same 68K run burst. Production interleaves
    // Z80 only after the M68K burst ends, but BUSACK must still be evaluated
    // at the current 68K access cycle, so this read must see the grant.
    val = env.bus.read8(0xA11100, 7);
    check((val & 0x01) == 0, "BUSACK granted during same M68K burst");

    endGroup();
}

// =====================================================================
// Test 9: Multiple BUSREQ toggles work correctly with proper timing
// =====================================================================
static void testBusreqMultipleToggles() {
    beginGroup("BUSREQ multiple toggles");

    Bus bus;
    BusTest::initMinimal(bus);
    BusTest::setZ80Reset(bus, false);  // Z80 running
    BusTest::resetZ80CycleCounter(bus);

    // First request: advance past 3-cycle grant threshold.
    BusTest::writeBusreq(bus, true);
    BusTest::updateBusAck(bus, 5);
    check(BusTest::getZ80BusAck(bus) == true, "First BUSACK granted");

    // First release: advance past 1-cycle resume threshold.
    BusTest::writeBusreq(bus, false);
    BusTest::updateBusAck(bus, 2);
    check(BusTest::getZ80BusAck(bus) == false, "First release complete");

    // Second request: fresh assert, fresh grant.
    BusTest::writeBusreq(bus, true);
    BusTest::updateBusAck(bus, 5);
    check(BusTest::getZ80BusAck(bus) == true, "Second BUSACK granted");

    // Second release.
    BusTest::writeBusreq(bus, false);
    BusTest::updateBusAck(bus, 2);
    check(BusTest::getZ80BusAck(bus) == false, "Second release complete");

    endGroup();
}

static void testMappedSRAMOverridesOverlappingROM() {
    beginGroup("mapped SRAM overrides overlapping ROM");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "sram_overlap.bin";

    check(writeRomFile(romPath, makeOverlappingSRAMRom()), "writes overlapping SRAM ROM");

    Cartridge cartridge;
    check(cartridge.load(romPath.string().c_str()), "loads cartridge");

    Bus bus;
    BusTest::initMinimal(bus);
    bus.connectCartridge(&cartridge);
    check(bus.loadROM(romPath.string().c_str()), "loads bus ROM mirror");

    check(bus.read8(0x200001) == 0x22, "unmapped read sees ROM byte under SRAM");

    bus.write8(0xA130F1, 0x01);
    check(bus.read8(0xA130F0) == 0x01, "SRAM mapping is enabled");

    bus.write8(0x200001, 0x5A);
    check(bus.read8(0x200001) == 0x5A, "mapped SRAM byte read returns save RAM");
    check(bus.read16(0x200000) == 0xFF5A,
          "mapped odd-byte SRAM word read returns open high byte and SRAM low byte");

    bus.write8(0x200002, 0xA5);
    check(bus.read8(0x200002) == 0xFF, "mapped odd-byte SRAM ignores even byte lane");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testDirectSRAMAtRomEndDoesNotRequireMapper() {
    beginGroup("direct SRAM at ROM end does not require mapper");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "sram_direct.bin";

    check(writeRomFile(romPath, makeDirectSRAMAtRomEndRom()), "writes direct SRAM ROM");

    Cartridge cartridge;
    check(cartridge.load(romPath.string().c_str()), "loads cartridge");

    Bus bus;
    BusTest::initMinimal(bus);
    bus.connectCartridge(&cartridge);
    check(bus.loadROM(romPath.string().c_str()), "loads bus ROM mirror");

    check(bus.read16(0x1FFFFE) == 0xCAFE, "ROM remains readable before direct SRAM");
    check(bus.read8(0xA130F0) == 0x00, "SRAM mapper is not enabled");

    bus.write8(0x200001, 0x5A);
    check(bus.read8(0x200001) == 0x5A, "direct odd-byte SRAM write is visible without mapper");
    check(bus.read16(0x200000) == 0xFF5A,
          "direct odd-byte SRAM word read returns open high byte and SRAM low byte");

    bus.write16(0x200002, 0x1234);
    check(bus.read16(0x200002) == 0xFF34,
          "direct odd-byte SRAM word write stores the odd byte lane");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testQueuedZ80BankStallsDoNotRecursivelyClockZ80() {
    beginGroup("queued Z80 bank stalls do not recursively clock Z80");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_bank_stall_scheduler.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes scheduler ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads scheduler ROM");

    Bus& bus = genesis.getBus();
    Z80& z80 = genesis.getZ80();
    u8* z80Ram = BusTest::getZ80Ram(bus);
    std::memset(z80Ram, 0, 0x2000);

    const u8 program[] = {
        0x3A, 0x00, 0x10,       // loop: LD A,($1000)
        0x3C,                   // INC A
        0x32, 0x00, 0x10,       // LD ($1000),A
        0xC3, 0x00, 0x00        // JP loop
    };
    std::memcpy(z80Ram, program, sizeof(program));

    z80.reset();
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;
    bus.pendingM68KZ80BusStallCycles = 200000;

    genesis.runFrame();

    check(z80Ram[0x1000] == 0,
          "pre-queued 68K-side stalls do not give the Z80 another execution budget");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80YMWritesSyncDuringElapsedSlice() {
    beginGroup("Z80 YM writes sync during elapsed slice");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_ym_elapsed_slice.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes elapsed-slice ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads elapsed-slice ROM");

    Bus& bus = genesis.getBus();
    Z80& z80 = genesis.getZ80();
    u8* z80Ram = BusTest::getZ80Ram(bus);
    std::memset(z80Ram, 0, 0x2000);

    const u8 program[] = {
        0x3E, 0x22,             // loop: LD A,$22
        0x32, 0x00, 0x40,       // LD ($4000),A
        0x3E, 0x00,             // LD A,$00
        0x32, 0x01, 0x40,       // LD ($4001),A
        0xC3, 0x00, 0x00        // JP loop
    };
    std::memcpy(z80Ram, program, sizeof(program));

    z80.reset();
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;
    GenesisTimingTest::resetZ80AudioTrace(genesis);

    GenesisTimingTest::clockZ80AndYM(genesis, 4000);

    check(GenesisTimingTest::getYMWrites(genesis) > 0,
          "Z80 program writes YM during elapsed slice");
    check(GenesisTimingTest::getYMSyncCycles(genesis) > 0,
          "elapsed Z80 slice synchronizes YM before Z80 register writes");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80YMStatusReadsSyncDuringBusyPoll() {
    beginGroup("Z80 YM status reads sync during busy poll");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_ym_busy_poll.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes busy-poll ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads busy-poll ROM");

    Bus& bus = genesis.getBus();
    Z80& z80 = genesis.getZ80();
    u8* z80Ram = BusTest::getZ80Ram(bus);
    std::memset(z80Ram, 0, 0x2000);

    const u8 program[] = {
        0x3E, 0x22,             // loop: LD A,$22
        0x32, 0x00, 0x40,       // LD ($4000),A
        0x3E, 0x00,             // LD A,$00
        0x32, 0x01, 0x40,       // LD ($4001),A
        0x3A, 0x00, 0x40,       // wait: LD A,($4000)
        0xE6, 0x80,             // AND $80
        0x20, 0xF9,             // JR NZ,wait
        0x3A, 0x00, 0x10,       // LD A,($1000)
        0x3C,                   // INC A
        0x32, 0x00, 0x10,       // LD ($1000),A
        0xC3, 0x00, 0x00        // JP loop
    };
    std::memcpy(z80Ram, program, sizeof(program));

    z80.reset();
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;
    GenesisTimingTest::resetZ80AudioTrace(genesis);

    GenesisTimingTest::clockZ80AndYM(genesis, 4000);

    check(GenesisTimingTest::getYMWrites(genesis) > 0,
          "Z80 busy-poll program writes YM");
    check(z80Ram[0x1000] > 0,
          "YM busy-poll loop observes busy clear during the elapsed slice");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80YMInvalidStatusMirrorUsesLatch() {
    beginGroup("Z80 YM invalid status mirror uses latch");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_ym_status_mirror.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes YM status mirror ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads YM status mirror ROM");

    Bus& bus = genesis.getBus();
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;

    bus.z80Write(0x4000, 0x22);
    bus.z80Write(0x4001, 0x00);

    check((bus.z80Read(0x4000) & 0x80) != 0, "valid status read latches busy flag");

    GenesisTimingTest::clockYM(genesis, 192);
    check((bus.z80Read(0x4002) & 0x80) != 0,
          "invalid status mirror returns latched status after live busy clears");
    check((bus.z80Read(0x4000) & 0x80) == 0, "valid status read refreshes latch clear");
    check((bus.z80Read(0x4002) & 0x80) == 0, "invalid status mirror follows refreshed latch");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80InterruptPulseExpiresIfInterruptsDisabled() {
    beginGroup("Z80 interrupt pulse expires if interrupts disabled");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_interrupt_pulse.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes Z80 interrupt pulse ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads Z80 interrupt pulse ROM");

    Bus& bus = genesis.getBus();
    Z80& z80 = genesis.getZ80();
    u8* z80Ram = BusTest::getZ80Ram(bus);
    std::memset(z80Ram, 0, 0x2000);
    z80Ram[0] = 0x00; // NOP forever while IFF1 remains disabled.

    z80.reset();
    z80.state.iff1 = false;
    z80.state.iff2 = false;
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;
    GenesisTimingTest::resetZ80AudioTrace(genesis);

    GenesisTimingTest::assertZ80InterruptPulse(genesis);
    check(z80.state.irqLine, "Z80 V-int pulse asserts interrupt line");

    GenesisTimingTest::clockZ80AndYM(genesis, 4000);

    check(!z80.state.irqLine,
          "Z80 V-int pulse clears when not accepted before the pulse window ends");
    check(GenesisTimingTest::getZ80InterruptPulseCycles(genesis) == 0,
          "Z80 V-int pulse countdown reaches zero");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80VIntPulseIgnores68KVIntEnable() {
    beginGroup("Z80 V-int pulse ignores 68K V-int enable");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "z80_vint_independent.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes Z80 V-int ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads Z80 V-int ROM");

    Bus& bus = genesis.getBus();
    Z80& z80 = genesis.getZ80();
    u8* z80Ram = BusTest::getZ80Ram(bus);
    std::memset(z80Ram, 0, 0x2000);

    const u8 program[] = {
        0xFB,                   // EI
        0xC3, 0x01, 0x00        // loop: JP loop
    };
    std::memcpy(z80Ram, program, sizeof(program));

    const u8 handler[] = {
        0x3A, 0x00, 0x10,       // LD A,($1000)
        0x3C,                   // INC A
        0x32, 0x00, 0x10,       // LD ($1000),A
        0xFB,                   // EI
        0xED, 0x4D              // RETI
    };
    std::memcpy(z80Ram + 0x0038, handler, sizeof(handler));

    z80.reset();
    bus.z80Reset = false;
    bus.z80BusRequested = false;
    bus.z80BusAck = false;
    GenesisTimingTest::resetZ80AudioTrace(genesis);

    genesis.runFrame();

    check(z80Ram[0x1000] != 0,
          "Z80 receives V-int pulse even when VDP reg #1 V-int enable is clear");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testMasterClockShadowTracksExactLineBoundary() {
    beginGroup("master clock shadow exact line boundary");

    Genesis genesis;
    GenesisTimingTest::resetMasterClock(genesis);
    GenesisTimingTest::beginMasterClockScanline(genesis);

    GenesisTimingTest::syncMasterClockToM68KLineCycle(genesis, 488);
    check(GenesisTimingTest::getMasterCycle(genesis) == 3416,
          "488 elapsed 68K cycles map to 3416 master clocks inside the line");

    GenesisTimingTest::endMasterClockScanline(genesis);
    check(GenesisTimingTest::getMasterCycle(genesis) == Genesis::MASTER_CYCLES_PER_SCANLINE,
          "ending a scanline advances to the exact 3420-master-clock boundary");
    check(GenesisTimingTest::getScanlineStartMasterCycle(genesis) == Genesis::MASTER_CYCLES_PER_SCANLINE,
          "next scanline starts at the exact master-clock boundary");

    endGroup();
}

static void testMasterClockShadowAdvancesOneFrame() {
    beginGroup("master clock shadow frame advance");

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "genesis_bus_timing_test";
    std::filesystem::create_directories(dir);
    const std::filesystem::path romPath = dir / "master_clock_shadow_frame.bin";

    check(writeRomFile(romPath, makeZ80BankStallSchedulerRom()), "writes master-clock ROM");

    Genesis genesis;
    check(genesis.loadROM(romPath.string().c_str()), "loads master-clock ROM");
    GenesisTimingTest::resetMasterClock(genesis);

    const u64 expectedFrameMasters =
        static_cast<u64>(genesis.getScanlinesPerFrame()) * Genesis::MASTER_CYCLES_PER_SCANLINE;

    genesis.runFrame();
    check(GenesisTimingTest::getMasterCycle(genesis) == expectedFrameMasters,
          "one frame advances the shadow clock by scanlines * 3420 master clocks");

    genesis.runFrame();
    check(GenesisTimingTest::getMasterCycle(genesis) == expectedFrameMasters * 2,
          "two frames accumulate exact master-clock frame length");

    std::error_code ec;
    std::filesystem::remove(romPath, ec);
    std::filesystem::remove(dir, ec);

    endGroup();
}

static void testZ80InterruptPulseRecordsMasterWindow() {
    beginGroup("Z80 interrupt pulse records master window");

    Genesis genesis;
    GenesisTimingTest::resetMasterClock(genesis);
    GenesisTimingTest::beginMasterClockScanline(genesis);
    GenesisTimingTest::syncMasterClockToM68KLineCycle(genesis, 112);

    const u64 pulseStart = GenesisTimingTest::getMasterCycle(genesis);
    GenesisTimingTest::assertZ80InterruptPulse(genesis);

    check(GenesisTimingTest::getZ80InterruptPulseStartMasterCycle(genesis) == pulseStart,
          "Z80 interrupt pulse records the current master-cycle start");
    check(GenesisTimingTest::getZ80InterruptPulseEndMasterCycle(genesis) == pulseStart + 2573,
          "Z80 interrupt pulse records the measured 2573-master-clock end");

    endGroup();
}

static void testZ80InterruptPulseMasterSyncDoesNotPreemptZ80Slice() {
    beginGroup("Z80 interrupt pulse master sync does not preempt Z80 slice");

    Genesis genesis;
    GenesisTimingTest::resetZ80AudioTrace(genesis);
    GenesisTimingTest::resetMasterClock(genesis);
    GenesisTimingTest::beginMasterClockScanline(genesis);
    GenesisTimingTest::assertZ80InterruptPulse(genesis);

    GenesisTimingTest::syncMasterClockToM68KLineCycle(genesis, 368);
    check(GenesisTimingTest::getZ80InterruptLine(genesis),
          "Z80 interrupt line stays asserted until Z80 execution observes it");
    check(GenesisTimingTest::getZ80InterruptPulseCycles(genesis) > 0,
          "Z80 interrupt pulse countdown remains owned by Z80 elapsed cycles");
    check(GenesisTimingTest::getZ80InterruptPulseExpirations(genesis) == 0,
          "Z80 interrupt pulse is not expired by master sync alone");

    endGroup();
}

static void testZ80MasterClockFragmentsPreserveDivider() {
    beginGroup("Z80 master clock fragments preserve divider");

    Genesis genesis;
    GenesisTimingTest::resetZ80AudioTrace(genesis);
    GenesisTimingTest::haltZ80(genesis);

    GenesisTimingTest::clockZ80Master(genesis, 14);
    check(GenesisTimingTest::getZ80CycleAccum(genesis) == 14,
          "Z80 master clock keeps sub-divider remainder");
    check(GenesisTimingTest::getZ80CyclesBudgeted(genesis) == 0,
          "Z80 master clock does not budget a cycle before divider threshold");
    check(GenesisTimingTest::getZ80CyclesHalted(genesis) == 0,
          "Z80 halt path does not consume before divider threshold");

    GenesisTimingTest::clockZ80Master(genesis, 1);
    check(GenesisTimingTest::getZ80CycleAccum(genesis) == 0,
          "Z80 master clock clears remainder at divider threshold");
    check(GenesisTimingTest::getZ80CyclesBudgeted(genesis) == 1,
          "Z80 master clock budgets one Z80 cycle after 15 master clocks");
    check(GenesisTimingTest::getZ80CyclesHalted(genesis) == 1,
          "Z80 halted clock consumes the converted Z80 cycle");

    endGroup();
}

static void testVDPDataWriteStallsWhenFifoFull() {
    beginGroup("VDP data write full FIFO wait");

    ConnectedM68KVDPBus env;

    // Set data port target to VRAM write at address zero.
    env.vdp.writeControl(0x4000);
    env.vdp.writeControl(0x0000);

    for (int i = 0; i < 4; i++) {
        env.bus.write16(0xC00000, static_cast<u16>(0x1000 + i), 0);
    }
    check(env.vdp.isVDPFIFOFull(), "four data writes fill the VDP FIFO");

    const int cyclesBefore = env.cpu.state.cycles;
    env.bus.write16(0xC00000, 0x2000, 0);

    check(env.cpu.state.cycles > cyclesBefore,
          "fifth data write stalls the 68K until FIFO space is available");
    check(env.vdp.isVDPFIFOFull(),
          "fifth data write occupies the freed FIFO slot");
    check(env.vdp.readVRAM(8) == 0x00 && env.vdp.readVRAM(9) == 0x00,
          "fifth data write does not bypass FIFO and modify VRAM immediately");

    endGroup();
}

static void testVDPDataWriteAtLineBoundaryDoesNotDropWhenFifoFull() {
    beginGroup("VDP data write full FIFO at line boundary");

    ConnectedM68KVDPBus env;

    // Exhaust the current scanline so no same-line external slot can drain
    // the FIFO before the next data-port write.
    env.vdp.clockM68K(VDP_MAX_M68K_CYCLES);

    env.vdp.writeControl(0x4000); // VRAM write address $0000
    env.vdp.writeControl(0x0000);

    for (int i = 0; i < 4; i++) {
        env.bus.write16(0xC00000, static_cast<u16>(0x1000 + i), 0);
    }
    check(env.vdp.isVDPFIFOFull(), "four boundary writes fill the VDP FIFO");

    env.bus.write16(0xC00000, 0x2000, 0);

    for (int i = 0; i < 4; i++) {
        env.vdp.endScanline();
        env.vdp.beginScanline();
        env.vdp.clockM68K(VDP_MAX_M68K_CYCLES);
    }

    check(env.vdp.readVRAM(8) == 0x20 && env.vdp.readVRAM(9) == 0x00,
          "line-boundary FIFO-full write eventually reaches VRAM");

    endGroup();
}

static void testVDPControlWriteStarting68kDMAStallsCPU() {
    beginGroup("VDP control write 68K DMA wait");

    ConnectedM68KVDPBus env;

    env.bus.write16(0xFF0000, 0xABCD);

    env.vdp.writeControl(0x8110); // DMA enabled, display disabled
    env.vdp.writeControl(0x8C81); // H40 slot cadence
    env.vdp.writeControl(0x8F02); // auto-increment by one word
    env.vdp.writeControl(0x9301); // DMA length low: one word
    env.vdp.writeControl(0x9400); // DMA length high
    env.vdp.writeControl(0x9500); // source low: $FF0000 >> 1
    env.vdp.writeControl(0x9680); // source mid
    env.vdp.writeControl(0x977F); // source high, 68K -> VDP mode

    env.bus.write16(0xC00004, 0x4000, 0); // VRAM write address $0000
    const int cyclesBefore = env.cpu.state.cycles;
    env.bus.write16(0xC00004, 0x0080, 0); // DMA bit set

    check(env.cpu.state.cycles > cyclesBefore,
          "DMA-starting control write stalls the 68K");
    check(!env.vdp.is68kDMABusy(),
          "68K DMA source transfer completes before CPU continues");
    check(env.vdp.readVRAM(0) == 0x00 && env.vdp.readVRAM(1) == 0x00,
          "DMA write remains queued behind FIFO latency after CPU stall");

    env.vdp.clockM68K(VDP_MAX_M68K_CYCLES);
    check(env.vdp.readVRAM(0) == 0xAB && env.vdp.readVRAM(1) == 0xCD,
          "queued DMA write eventually reaches VRAM");

    endGroup();
}

static void testVDPControlWrite68kDMAWaitStopsAtLineBoundary() {
    beginGroup("VDP control write DMA line boundary");

    ConnectedM68KVDPBus env;

    env.bus.write16(0xFF0000, 0xABCD);
    env.bus.write16(0xFF0002, 0x1234);

    env.vdp.clockM68K(VDP_MAX_M68K_CYCLES - 1);
    env.vdp.writeControl(0x8110); // DMA enabled, display disabled
    env.vdp.writeControl(0x8F02); // auto-increment by one word
    env.vdp.writeControl(0x9302); // DMA length low: two words
    env.vdp.writeControl(0x9400); // DMA length high
    env.vdp.writeControl(0x9500); // source low: $FF0000 >> 1
    env.vdp.writeControl(0x9680); // source mid
    env.vdp.writeControl(0x977F); // source high, 68K -> VDP mode

    env.bus.write16(0xC00004, 0x4000, 0); // VRAM write address $0000
    env.bus.write16(0xC00004, 0x0080, 0); // DMA bit set

    check(env.cpu.state.cycles <= 1,
          "DMA wait does not spin past the scanline budget");
    check(env.vdp.is68kDMABusy(),
          "DMA that cannot finish this line remains active for the scheduler");

    endGroup();
}

// =====================================================================
int main() {
    printf("=== Bus Timing Test Suite ===\n\n");

    testZ80BankReadTiming();
    testZ80BankReadStallsM68K();
    testZ80BankWriteTiming();
    testZ80BankWriteStallsM68K();
    testM68KZ80AreaReadWait();
    testM68KZ80AreaWriteWait();
    testM68KZ80Area16BitWait();
    testBusreqAckDelay();
    testBusreqResumeDelay();
    testBusackRegisterRead();
    testBusackRegisterReadWhileResetAsserted();
    testBusackPollProgressesWithinM68KBurst();
    testBusreqMultipleToggles();
    testMappedSRAMOverridesOverlappingROM();
    testDirectSRAMAtRomEndDoesNotRequireMapper();
    testQueuedZ80BankStallsDoNotRecursivelyClockZ80();
    testZ80YMWritesSyncDuringElapsedSlice();
    testZ80YMStatusReadsSyncDuringBusyPoll();
    testZ80YMInvalidStatusMirrorUsesLatch();
    testZ80InterruptPulseExpiresIfInterruptsDisabled();
    testZ80VIntPulseIgnores68KVIntEnable();
    testMasterClockShadowTracksExactLineBoundary();
    testMasterClockShadowAdvancesOneFrame();
    testZ80InterruptPulseRecordsMasterWindow();
    testZ80InterruptPulseMasterSyncDoesNotPreemptZ80Slice();
    testZ80MasterClockFragmentsPreserveDivider();
    testVDPDataWriteStallsWhenFifoFull();
    testVDPDataWriteAtLineBoundaryDoesNotDropWhenFifoFull();
    testVDPControlWriteStarting68kDMAStallsCPU();
    testVDPControlWrite68kDMAWaitStopsAtLineBoundary();

    printf("\n=== Results: %d/%d passed", passedTests, totalTests);
    if (failedTests > 0) {
        printf(", %d FAILED", failedTests);
    }
    printf(" ===\n");

    return failedTests > 0 ? 1 : 0;
}
