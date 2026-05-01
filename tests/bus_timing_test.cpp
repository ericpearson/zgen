// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// Bus timing test suite
// Tests Z80 bank access timing and 68K Z80-area wait states

#include "memory/bus.h"
#include "cpu/m68k.h"
#include "cpu/z80.h"
#include <cstdio>
#include <cstring>

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
    testBusackPollProgressesWithinM68KBurst();
    testBusreqMultipleToggles();

    printf("\n=== Results: %d/%d passed", passedTests, totalTests);
    if (failedTests > 0) {
        printf(", %d FAILED", failedTests);
    }
    printf(" ===\n");

    return failedTests > 0 ? 1 : 0;
}
