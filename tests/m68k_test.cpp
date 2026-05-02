// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// M68K CPU instruction test suite
// Self-contained exerciser that stubs the Bus and tests instructions directly.

#include "cpu/m68k.h"
#include "memory/bus.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdlib>

// Flat 16MB memory for standalone M68K testing
static u8 memory[0x1000000]; // 16MB
static int lastRead8PartialCycles = -1;
static int lastRead16PartialCycles = -1;
struct Read16Event {
    u32 addr;
    u16 val;
    int partialCycles;
};
struct Read8Event {
    u32 addr;
    u8 val;
    int partialCycles;
};
struct Write16Event {
    u32 addr;
    u16 val;
    int partialCycles;
};
struct Write8Event {
    u32 addr;
    u8 val;
    int partialCycles;
};
static Read16Event read16Events[16];
static int read16EventCount = 0;
static Read8Event read8Events[16];
static int read8EventCount = 0;
static Write16Event write16Events[8];
static int write16EventCount = 0;
static Write8Event write8Events[8];
static int write8EventCount = 0;

// --- Minimal Bus stubs ---
Bus::Bus() : z80Bank(0), z80BusRequested(false), z80Reset(false),
             m68k(nullptr), z80(nullptr), vdp(nullptr), psg(nullptr),
             ym2612(nullptr), cartridge(nullptr), controllers{nullptr, nullptr},
             rom(nullptr), romSize(0), sramMapped(false) {
    memset(ram, 0, sizeof(ram));
    memset(z80Ram, 0, sizeof(z80Ram));
    memset(ioData, 0, sizeof(ioData));
    memset(ioCtrl, 0, sizeof(ioCtrl));
    memset(padState, 0, sizeof(padState));
    memset(padState6, 0, sizeof(padState6));
    memset(thCounter, 0, sizeof(thCounter));
    memset(prevTH, 0, sizeof(prevTH));
    memset(romBankRegs, 0, sizeof(romBankRegs));
}
Bus::~Bus() {}
void Bus::reset() {}
u8 Bus::z80Read(u16) { return 0; }
void Bus::z80Write(u16, u8) {}
u8 Bus::read8(u32 addr) { return memory[addr & 0xFFFFFF]; }
u8 Bus::read8(u32 addr, int partialCycles) {
    lastRead8PartialCycles = partialCycles;
    addr &= 0xFFFFFF;
    const u8 val = memory[addr];
    if (read8EventCount < static_cast<int>(sizeof(read8Events) / sizeof(read8Events[0]))) {
        read8Events[read8EventCount++] = {addr, val, partialCycles};
    }
    return val;
}
u8 Bus::debugPeek8(u32 addr) const {
    return memory[addr & 0xFFFFFF];
}
u16 Bus::read16(u32 addr) {
    addr &= 0xFFFFFE;
    return (static_cast<u16>(memory[addr]) << 8) | memory[addr + 1];
}
u16 Bus::read16(u32 addr, int partialCycles) {
    lastRead16PartialCycles = partialCycles;
    addr &= 0xFFFFFE;
    const u16 val = (static_cast<u16>(memory[addr]) << 8) | memory[addr + 1];
    if (read16EventCount < static_cast<int>(sizeof(read16Events) / sizeof(read16Events[0]))) {
        read16Events[read16EventCount++] = {addr, val, partialCycles};
    }
    return val;
}
u16 Bus::debugPeek16(u32 addr) const {
    addr &= 0xFFFFFE;
    return (static_cast<u16>(memory[addr]) << 8) | memory[addr + 1];
}
u32 Bus::debugPeek32(u32 addr) const {
    return (static_cast<u32>(debugPeek16(addr)) << 16) | debugPeek16(addr + 2);
}
u32 Bus::getM68KPC() const { return 0; }
void Bus::write8(u32 addr, u8 val) { memory[addr & 0xFFFFFF] = val; }
void Bus::write8(u32 addr, u8 val, int partialCycles) {
    if (write8EventCount < static_cast<int>(sizeof(write8Events) / sizeof(write8Events[0]))) {
        write8Events[write8EventCount++] = {addr & 0xFFFFFF, val, partialCycles};
    }
    write8(addr, val);
}
void Bus::write16(u32 addr, u16 val) {
    addr &= 0xFFFFFE;
    memory[addr] = val >> 8;
    memory[addr + 1] = val & 0xFF;
}
void Bus::write16(u32 addr, u16 val, int partialCycles) {
    if (write16EventCount < static_cast<int>(sizeof(write16Events) / sizeof(write16Events[0]))) {
        write16Events[write16EventCount++] = {addr & 0xFFFFFE, val, partialCycles};
    }
    write16(addr, val);
}
bool Bus::loadROM(const char*) { return false; }
void Bus::setButtonState(int, int, bool) {}
void Bus::resetControllerCounters() {}
bool Bus::isM68KBusStalled() const { return false; }
u8 Bus::readControllerData(int) { return 0xFF; }

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

// Helper to write a word at an address in big-endian
static void writeWord(u32 addr, u16 val) {
    memory[addr] = val >> 8;
    memory[addr + 1] = val & 0xFF;
}

static void writeLong(u32 addr, u32 val) {
    memory[addr] = (val >> 24) & 0xFF;
    memory[addr + 1] = (val >> 16) & 0xFF;
    memory[addr + 2] = (val >> 8) & 0xFF;
    memory[addr + 3] = val & 0xFF;
}

static Bus bus;
static M68K cpu;

// Reset CPU and memory, place instruction(s) at address 0x1000
static void resetCPU() {
    memset(memory, 0, sizeof(memory));
    lastRead8PartialCycles = -1;
    lastRead16PartialCycles = -1;
    read8EventCount = 0;
    read16EventCount = 0;
    write16EventCount = 0;
    write8EventCount = 0;
    // Set up vector table: SSP=0x800000, PC=0x1000
    writeLong(0x000000, 0x00800000); // Initial SSP
    writeLong(0x000004, 0x00001000); // Initial PC
    // Exception vectors point to a safe RTE
    for (int i = 2; i < 64; i++) {
        writeLong(i * 4, 0x00002000); // all exceptions go to 0x2000
    }
    // Place RTE at 0x2000
    writeWord(0x2000, 0x4E73); // RTE

    cpu.reset();
    // Clear cycles
    cpu.state.sr = 0x2700; // Supervisor, all ints masked
    cpu.state.cycles = 0;
}

// Place instruction word(s) at PC and execute one instruction
static int placeAndExec(u16 opcode) {
    writeWord(cpu.state.pc, opcode);
    return cpu.execute();
}

static int placeAndExec2(u16 opcode, u16 ext) {
    writeWord(cpu.state.pc, opcode);
    writeWord(cpu.state.pc + 2, ext);
    return cpu.execute();
}

static int placeAndExec3(u16 opcode, u16 ext1, u16 ext2) {
    writeWord(cpu.state.pc, opcode);
    writeWord(cpu.state.pc + 2, ext1);
    writeWord(cpu.state.pc + 4, ext2);
    return cpu.execute();
}

static int placeAndExec4(u16 opcode, u16 ext1, u16 ext2, u16 ext3) {
    writeWord(cpu.state.pc, opcode);
    writeWord(cpu.state.pc + 2, ext1);
    writeWord(cpu.state.pc + 4, ext2);
    writeWord(cpu.state.pc + 6, ext3);
    return cpu.execute();
}

static void test_operand_read_timing() {
    beginGroup("operand read timing");

    resetCPU();
    writeWord(0x2000, 0x1234);
    placeAndExec2(0x3038, 0x2000); // MOVE.W $2000.W,D0
    check(lastRead16PartialCycles == 8, "MOVE.W abs.w samples operand after opcode and extension fetches");

    resetCPU();
    writeWord(0x3000, 0x5678);
    placeAndExec3(0x3039, 0x0000, 0x3000); // MOVE.W $00003000.L,D0
    check(lastRead16PartialCycles == 12, "MOVE.W abs.l samples operand after opcode and address fetches");

    resetCPU();
    cpu.state.a[0] = 0x4000;
    writeWord(0x4000, 0x9ABC);
    placeAndExec(0x3010); // MOVE.W (A0),D0
    check(lastRead16PartialCycles == 4, "MOVE.W (An) samples operand after the opcode fetch");

    resetCPU();
    memory[0xC00008] = 0xB6;
    placeAndExec4(0x0C39, 0x00B7, 0x00C0, 0x0008); // CMPI.B #$B7,$00C00008.L
    check(lastRead8PartialCycles == 16, "CMPI.B #imm,abs.l samples operand after immediate and address fetches");

    endGroup();
}

static void test_operand_write_timing() {
    beginGroup("operand write timing");

    resetCPU();
    cpu.state.a[6] = 0x5000;
    writeLong(0x5000, 0x008BFF90);
    placeAndExec3(0x23DE, 0x00C0, 0x0000); // MOVE.L (A6)+,$00C00000.L
    check(write16EventCount == 2, "MOVE.L (A6)+,abs.l emits two word writes");
    check(write16Events[0].addr == 0xC00000 && write16Events[0].val == 0x008B && write16Events[0].partialCycles == 20,
          "MOVE.L (A6)+,abs.l high word write follows source read and address fetches");
    check(write16Events[1].addr == 0xC00002 && write16Events[1].val == 0xFF90 && write16Events[1].partialCycles == 24,
          "MOVE.L (A6)+,abs.l low word write follows high word write");

    endGroup();
}

static void test_immediate_memory_alu_timing() {
    beginGroup("immediate memory ALU timing");

    resetCPU();
    writeWord(0xC00000, 0x0002);
    placeAndExec4(0x0679, 0x0001, 0x00C0, 0x0000); // ADDI.W #$0001,$00C00000.L
    check(lastRead16PartialCycles == 16, "ADDI.W #imm,abs.l reads destination after immediate and address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 20,
          "ADDI.W #imm,abs.l writes destination after read-modify slot");

    resetCPU();
    writeWord(0xC00000, 0x0002);
    placeAndExec4(0x0079, 0x0001, 0x00C0, 0x0000); // ORI.W #$0001,$00C00000.L
    check(lastRead16PartialCycles == 16, "ORI.W #imm,abs.l reads destination after immediate and address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 20,
          "ORI.W #imm,abs.l writes destination after read-modify slot");

    endGroup();
}

static void test_static_bit_memory_timing() {
    beginGroup("static bit memory timing");

    resetCPU();
    memory[0xC00008] = 0x02;
    placeAndExec4(0x0839, 0x0001, 0x00C0, 0x0008); // BTST #1,$00C00008.L
    check(lastRead8PartialCycles == 16, "BTST #imm,abs.l reads destination after bit number and address fetches");
    check(write8EventCount == 0, "BTST #imm,abs.l does not write destination");

    resetCPU();
    memory[0xC00008] = 0x02;
    placeAndExec4(0x08F9, 0x0000, 0x00C0, 0x0008); // BSET #0,$00C00008.L
    check(lastRead8PartialCycles == 16, "BSET #imm,abs.l reads destination after bit number and address fetches");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00008 &&
              write8Events[0].val == 0x03 && write8Events[0].partialCycles == 20,
          "BSET #imm,abs.l writes destination after read-modify slot");

    endGroup();
}

static void test_read_modify_write_memory_timing() {
    beginGroup("read-modify-write memory timing");

    resetCPU();
    writeWord(0xC00000, 0x00F0);
    placeAndExec3(0x4679, 0x00C0, 0x0000); // NOT.W $00C00000.L
    check(lastRead16PartialCycles == 12, "NOT.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0xFF0F && write16Events[0].partialCycles == 16,
          "NOT.W abs.l writes destination after read-modify slot");

    resetCPU();
    writeWord(0xC00000, 0x00F0);
    placeAndExec3(0x4279, 0x00C0, 0x0000); // CLR.W $00C00000.L
    check(lastRead16PartialCycles == 12, "CLR.W abs.l performs timed dummy read after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0000 && write16Events[0].partialCycles == 16,
          "CLR.W abs.l writes destination after dummy read slot");

    resetCPU();
    writeWord(0xC00000, 0x0002);
    placeAndExec3(0x5279, 0x00C0, 0x0000); // ADDQ.W #1,$00C00000.L
    check(lastRead16PartialCycles == 12, "ADDQ.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 16,
          "ADDQ.W abs.l writes destination after read-modify slot");

    resetCPU();
    writeWord(0xC00000, 0x0002);
    placeAndExec3(0x5379, 0x00C0, 0x0000); // SUBQ.W #1,$00C00000.L
    check(lastRead16PartialCycles == 12, "SUBQ.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0001 && write16Events[0].partialCycles == 16,
          "SUBQ.W abs.l writes destination after read-modify slot");

    resetCPU();
    cpu.state.d[0] = 0x0001;
    writeWord(0xC00000, 0x0002);
    placeAndExec3(0xD179, 0x00C0, 0x0000); // ADD.W D0,$00C00000.L
    check(lastRead16PartialCycles == 12, "ADD.W Dn,abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 16,
          "ADD.W Dn,abs.l writes destination after read-modify slot");

    resetCPU();
    cpu.state.d[0] = 0x0001;
    writeWord(0xC00000, 0x0002);
    placeAndExec3(0x8179, 0x00C0, 0x0000); // OR.W D0,$00C00000.L
    check(lastRead16PartialCycles == 12, "OR.W Dn,abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 16,
          "OR.W Dn,abs.l writes destination after read-modify slot");

    endGroup();
}

static void test_memory_shift_rotate_timing() {
    beginGroup("memory shift/rotate timing");

    resetCPU();
    writeWord(0xC00000, 0x4001);
    placeAndExec3(0xE1F9, 0x00C0, 0x0000); // ASL.W $00C00000.L
    check(lastRead16PartialCycles == 12, "ASL.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x8002 && write16Events[0].partialCycles == 16,
          "ASL.W abs.l writes destination after read-modify slot");

    resetCPU();
    writeWord(0xC00000, 0x8001);
    placeAndExec3(0xE2F9, 0x00C0, 0x0000); // LSR.W $00C00000.L
    check(lastRead16PartialCycles == 12, "LSR.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x4000 && write16Events[0].partialCycles == 16,
          "LSR.W abs.l writes destination after read-modify slot");

    resetCPU();
    writeWord(0xC00000, 0x8001);
    placeAndExec3(0xE7F9, 0x00C0, 0x0000); // ROL.W $00C00000.L
    check(lastRead16PartialCycles == 12, "ROL.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 16,
          "ROL.W abs.l writes destination after read-modify slot");

    resetCPU();
    cpu.state.sr |= FLAG_X;
    writeWord(0xC00000, 0x0002);
    placeAndExec3(0xE4F9, 0x00C0, 0x0000); // ROXR.W $00C00000.L
    check(lastRead16PartialCycles == 12, "ROXR.W abs.l reads destination after address fetches");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x8001 && write16Events[0].partialCycles == 16,
          "ROXR.W abs.l writes destination after read-modify slot");

    endGroup();
}

static void test_decimal_memory_timing() {
    beginGroup("decimal memory timing");

    resetCPU();
    memory[0xC00000] = 0x25;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec3(0x4839, 0x00C0, 0x0000); // NBCD $00C00000.L
    check(lastRead8PartialCycles == 12, "NBCD abs.l reads destination after address fetches");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00000 &&
              write8Events[0].val == 0x75 && write8Events[0].partialCycles == 16,
          "NBCD abs.l writes destination after read-modify slot");

    endGroup();
}

static void test_x_memory_predecrement_timing() {
    beginGroup("extend memory predecrement timing");

    resetCPU();
    cpu.state.a[0] = 0xC00002;
    cpu.state.a[1] = 0xC00012;
    memory[0xC00001] = 0x15;
    memory[0xC00011] = 0x27;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xC109); // ABCD -(A1),-(A0)
    check(read8EventCount == 2, "ABCD predecrement reads source and destination bytes");
    check(read8Events[0].addr == 0xC00011 && read8Events[0].val == 0x27 &&
              read8Events[0].partialCycles == 6,
          "ABCD predecrement reads source byte after source predecrement slot");
    check(read8Events[1].addr == 0xC00001 && read8Events[1].val == 0x15 &&
              read8Events[1].partialCycles == 10,
          "ABCD predecrement reads destination byte in the next bus slot");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00001 &&
              write8Events[0].val == 0x42 && write8Events[0].partialCycles == 14,
          "ABCD predecrement writes destination byte after decimal add");

    resetCPU();
    cpu.state.a[7] = 0xC00004;
    cpu.state.a[1] = 0xC00012;
    memory[0xC00002] = 0x15;
    memory[0xC00011] = 0x27;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xCF09); // ABCD -(A1),-(A7)
    check(cpu.state.a[7] == 0xC00002, "ABCD byte predecrement treats A7 as word-aligned");
    check(read8EventCount == 2 && read8Events[1].addr == 0xC00002 &&
              read8Events[1].partialCycles == 10,
          "ABCD predecrement reads A7 destination after subtracting two bytes");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00002 &&
              write8Events[0].val == 0x42 && write8Events[0].partialCycles == 14,
          "ABCD predecrement writes A7 destination after decimal add");

    resetCPU();
    cpu.state.a[0] = 0xC00002;
    cpu.state.a[1] = 0xC00012;
    memory[0xC00001] = 0x42;
    memory[0xC00011] = 0x15;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x8109); // SBCD -(A1),-(A0)
    check(read8EventCount == 2, "SBCD predecrement reads source and destination bytes");
    check(read8Events[0].addr == 0xC00011 && read8Events[0].val == 0x15 &&
              read8Events[0].partialCycles == 6,
          "SBCD predecrement reads source byte after source predecrement slot");
    check(read8Events[1].addr == 0xC00001 && read8Events[1].val == 0x42 &&
              read8Events[1].partialCycles == 10,
          "SBCD predecrement reads destination byte in the next bus slot");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00001 &&
              write8Events[0].val == 0x27 && write8Events[0].partialCycles == 14,
          "SBCD predecrement writes destination byte after decimal subtract");

    resetCPU();
    cpu.state.a[0] = 0xC00002;
    cpu.state.a[1] = 0xC00012;
    memory[0xC00001] = 0x02;
    memory[0xC00011] = 0x01;
    cpu.state.sr &= ~FLAG_X;
    int cycles = placeAndExec(0xD109); // ADDX.B -(A1),-(A0)
    check(cycles == 18, "ADDX.B predecrement charges full memory-mode cycles");
    check(read8EventCount == 2, "ADDX.B predecrement reads source and destination bytes");
    check(read8Events[0].addr == 0xC00011 && read8Events[0].val == 0x01 &&
              read8Events[0].partialCycles == 6,
          "ADDX.B predecrement reads source byte after source predecrement slot");
    check(read8Events[1].addr == 0xC00001 && read8Events[1].val == 0x02 &&
              read8Events[1].partialCycles == 10,
          "ADDX.B predecrement reads destination byte in the next bus slot");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00001 &&
              write8Events[0].val == 0x03 && write8Events[0].partialCycles == 14,
          "ADDX.B predecrement writes destination byte after extended add");

    resetCPU();
    cpu.state.a[0] = 0xC00002;
    cpu.state.a[1] = 0xC00012;
    memory[0xC00001] = 0x03;
    memory[0xC00011] = 0x01;
    cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x9109); // SUBX.B -(A1),-(A0)
    check(cycles == 18, "SUBX.B predecrement charges full memory-mode cycles");
    check(read8EventCount == 2, "SUBX.B predecrement reads source and destination bytes");
    check(read8Events[0].addr == 0xC00011 && read8Events[0].val == 0x01 &&
              read8Events[0].partialCycles == 6,
          "SUBX.B predecrement reads source byte after source predecrement slot");
    check(read8Events[1].addr == 0xC00001 && read8Events[1].val == 0x03 &&
              read8Events[1].partialCycles == 10,
          "SUBX.B predecrement reads destination byte in the next bus slot");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00001 &&
              write8Events[0].val == 0x02 && write8Events[0].partialCycles == 14,
          "SUBX.B predecrement writes destination byte after extended subtract");

    resetCPU();
    cpu.state.a[0] = 0xC00004;
    cpu.state.a[1] = 0xC00014;
    writeLong(0xC00000, 0x00000002);
    writeLong(0xC00010, 0x00000001);
    cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0xD189); // ADDX.L -(A1),-(A0)
    check(cycles == 30, "ADDX.L predecrement charges full memory-mode cycles");
    check(read16EventCount == 4, "ADDX.L predecrement reads two source and two destination words");
    check(read16Events[0].addr == 0xC00010 && read16Events[0].val == 0x0000 &&
              read16Events[0].partialCycles == 6,
          "ADDX.L predecrement reads source high word first");
    check(read16Events[1].addr == 0xC00012 && read16Events[1].val == 0x0001 &&
              read16Events[1].partialCycles == 10,
          "ADDX.L predecrement reads source low word second");
    check(read16Events[2].addr == 0xC00000 && read16Events[2].val == 0x0000 &&
              read16Events[2].partialCycles == 14,
          "ADDX.L predecrement reads destination high word third");
    check(read16Events[3].addr == 0xC00002 && read16Events[3].val == 0x0002 &&
              read16Events[3].partialCycles == 18,
          "ADDX.L predecrement reads destination low word fourth");
    check(write16EventCount == 2, "ADDX.L predecrement writes two destination words");
    check(write16Events[0].addr == 0xC00000 && write16Events[0].val == 0x0000 &&
              write16Events[0].partialCycles == 22,
          "ADDX.L predecrement writes destination high word after reads");
    check(write16Events[1].addr == 0xC00002 && write16Events[1].val == 0x0003 &&
              write16Events[1].partialCycles == 26,
          "ADDX.L predecrement writes destination low word last");

    resetCPU();
    cpu.state.a[0] = 0xC00004;
    cpu.state.a[1] = 0xC00014;
    writeLong(0xC00000, 0x00000003);
    writeLong(0xC00010, 0x00000001);
    cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x9189); // SUBX.L -(A1),-(A0)
    check(cycles == 30, "SUBX.L predecrement charges full memory-mode cycles");
    check(read16EventCount == 4, "SUBX.L predecrement reads two source and two destination words");
    check(read16Events[0].addr == 0xC00010 && read16Events[0].val == 0x0000 &&
              read16Events[0].partialCycles == 6,
          "SUBX.L predecrement reads source high word first");
    check(read16Events[1].addr == 0xC00012 && read16Events[1].val == 0x0001 &&
              read16Events[1].partialCycles == 10,
          "SUBX.L predecrement reads source low word second");
    check(read16Events[2].addr == 0xC00000 && read16Events[2].val == 0x0000 &&
              read16Events[2].partialCycles == 14,
          "SUBX.L predecrement reads destination high word third");
    check(read16Events[3].addr == 0xC00002 && read16Events[3].val == 0x0003 &&
              read16Events[3].partialCycles == 18,
          "SUBX.L predecrement reads destination low word fourth");
    check(write16EventCount == 2, "SUBX.L predecrement writes two destination words");
    check(write16Events[0].addr == 0xC00000 && write16Events[0].val == 0x0000 &&
              write16Events[0].partialCycles == 22,
          "SUBX.L predecrement writes destination high word after reads");
    check(write16Events[1].addr == 0xC00002 && write16Events[1].val == 0x0002 &&
              write16Events[1].partialCycles == 26,
          "SUBX.L predecrement writes destination low word last");

    endGroup();
}

static void test_data_movement_memory_timing() {
    beginGroup("data movement memory timing");

    resetCPU();
    cpu.state.d[0] = 0x11223344;
    u32 sp = cpu.state.a[7];
    int cycles = placeAndExec(0x2F00); // MOVE.L D0,-(A7)
    check(cycles == 12, "MOVE.L Dn,-(An) charges register-source predecrement cycles");
    check(cpu.state.a[7] == sp - 4, "MOVE.L Dn,-(An) predecrements destination by long size");
    check(write16EventCount == 2, "MOVE.L Dn,-(An) emits two timed destination writes");
    check(write16Events[0].addr == sp - 2 && write16Events[0].val == 0x3344 &&
              write16Events[0].partialCycles == 4,
          "MOVE.L Dn,-(An) writes low word first");
    check(write16Events[1].addr == sp - 4 && write16Events[1].val == 0x1122 &&
              write16Events[1].partialCycles == 8,
          "MOVE.L Dn,-(An) writes high word second");

    resetCPU();
    cpu.state.a[0] = 0x4000;
    writeLong(0x4000, 0x11223344);
    sp = cpu.state.a[7];
    cycles = placeAndExec(0x2F10); // MOVE.L (A0),-(A7)
    check(cycles == 20, "MOVE.L (An),-(An) charges source read and destination write cycles");
    check(read16EventCount == 2, "MOVE.L (An),-(An) emits two timed source reads");
    check(read16Events[0].addr == 0x4000 && read16Events[0].val == 0x1122 &&
              read16Events[0].partialCycles == 4,
          "MOVE.L (An),-(An) reads source high word first");
    check(read16Events[1].addr == 0x4002 && read16Events[1].val == 0x3344 &&
              read16Events[1].partialCycles == 8,
          "MOVE.L (An),-(An) reads source low word second");
    check(write16EventCount == 2, "MOVE.L (An),-(An) emits two timed destination writes");
    check(write16Events[0].addr == sp - 2 && write16Events[0].val == 0x3344 &&
              write16Events[0].partialCycles == 12,
          "MOVE.L (An),-(An) writes destination low word first");
    check(write16Events[1].addr == sp - 4 && write16Events[1].val == 0x1122 &&
              write16Events[1].partialCycles == 16,
          "MOVE.L (An),-(An) writes destination high word second");

    resetCPU();
    cpu.state.sr = 0x2704;
    writeWord(0xC00000, 0xAAAA);
    cycles = placeAndExec3(0x40F9, 0x00C0, 0x0000); // MOVE SR,$00C00000.L
    check(cycles == 20, "MOVE SR,abs.l charges 8+EA cycles");
    check(read16EventCount == 1 && read16Events[0].addr == 0xC00000 &&
              read16Events[0].val == 0xAAAA && read16Events[0].partialCycles == 12,
          "MOVE SR,abs.l dummy-reads destination before write");
    check(write16EventCount == 1 && write16Events[0].addr == 0xC00000 &&
              write16Events[0].val == 0x2704 && write16Events[0].partialCycles == 16,
          "MOVE SR,abs.l writes SR after destination dummy read");

    resetCPU();
    cpu.state.a[0] = 0x12345678;
    sp = cpu.state.a[7];
    cycles = placeAndExec(0x4850); // PEA (A0)
    check(cycles == 12, "PEA (An) charges documented stack-write cycles");
    check(cpu.state.a[7] == sp - 4, "PEA (An) pushes a longword");
    check(write16EventCount == 2, "PEA (An) emits two timed stack writes");
    check(write16Events[0].addr == sp - 2 && write16Events[0].val == 0x5678 &&
              write16Events[0].partialCycles == 4,
          "PEA (An) writes low word first");
    check(write16Events[1].addr == sp - 4 && write16Events[1].val == 0x1234 &&
              write16Events[1].partialCycles == 8,
          "PEA (An) writes high word second");

    endGroup();
}

static void test_predecrement_read_timing() {
    beginGroup("predecrement read timing");

    resetCPU();
    cpu.state.a[0] = 0x4002;
    cpu.state.d[0] = 3;
    writeWord(0x4000, 5);
    placeAndExec(0xC0E0); // MULU.W -(A0),D0
    check(read16EventCount == 1 && read16Events[0].addr == 0x4000 &&
              read16Events[0].val == 5 && read16Events[0].partialCycles == 6,
          "MULU.W -(An),Dn reads source after predecrement idle slot");

    resetCPU();
    cpu.state.a[0] = 0x4001;
    memory[0x4000] = 0x25;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x4820); // NBCD -(A0)
    check(read8EventCount == 1 && read8Events[0].addr == 0x4000 &&
              read8Events[0].val == 0x25 && read8Events[0].partialCycles == 6,
          "NBCD -(An) reads destination after predecrement idle slot");
    check(write8EventCount == 1 && write8Events[0].addr == 0x4000 &&
              write8Events[0].val == 0x75 && write8Events[0].partialCycles == 10,
          "NBCD -(An) writes destination after predecrement read-modify slot");

    resetCPU();
    cpu.state.a[0] = 0x4002;
    writeWord(0x4000, 0x0002);
    placeAndExec2(0x0660, 0x0001); // ADDI.W #1,-(A0)
    check(read16EventCount == 1 && read16Events[0].addr == 0x4000 &&
              read16Events[0].val == 0x0002 && read16Events[0].partialCycles == 10,
          "ADDI.W #imm,-(An) reads destination after immediate and predecrement slots");
    check(write16EventCount == 1 && write16Events[0].addr == 0x4000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 14,
          "ADDI.W #imm,-(An) writes destination after read-modify slot");

    resetCPU();
    cpu.state.d[0] = 0x0001;
    cpu.state.a[1] = 0x4002;
    writeWord(0x4000, 0x0002);
    placeAndExec(0xD161); // ADD.W D0,-(A1)
    check(read16EventCount == 1 && read16Events[0].addr == 0x4000 &&
              read16Events[0].val == 0x0002 && read16Events[0].partialCycles == 6,
          "ADD.W Dn,-(An) reads destination after predecrement idle slot");
    check(write16EventCount == 1 && write16Events[0].addr == 0x4000 &&
              write16Events[0].val == 0x0003 && write16Events[0].partialCycles == 10,
          "ADD.W Dn,-(An) writes destination after read-modify slot");

    endGroup();
}

static void test_tas_memory_timing() {
    beginGroup("TAS memory timing");

    resetCPU();
    memory[0xC00000] = 0x42;
    int cycles = placeAndExec3(0x4AF9, 0x00C0, 0x0000); // TAS $00C00000.L
    check(cycles == 22, "TAS abs.l charges 10+EA cycles");
    check(read8EventCount == 1 && read8Events[0].addr == 0xC00000 &&
              read8Events[0].val == 0x42 && read8Events[0].partialCycles == 12,
          "TAS abs.l reads destination byte after address fetches");
    check(write8EventCount == 0, "TAS memory writeback remains suppressed on Genesis");
    check(memory[0xC00000] == 0x42, "TAS memory value is unchanged when writeback is suppressed");

    endGroup();
}

static void test_scc_memory_timing() {
    beginGroup("Scc memory timing");

    resetCPU();
    cpu.state.sr |= FLAG_Z;
    memory[0xC00000] = 0x42;
    int cycles = placeAndExec3(0x57F9, 0x00C0, 0x0000); // SEQ $00C00000.L
    check(cycles == 20, "Scc abs.l charges 8+EA cycles");
    check(read8EventCount == 1 && read8Events[0].addr == 0xC00000 &&
              read8Events[0].val == 0x42 && read8Events[0].partialCycles == 12,
          "Scc abs.l dummy-reads destination before write");
    check(write8EventCount == 1 && write8Events[0].addr == 0xC00000 &&
              write8Events[0].val == 0xFF && write8Events[0].partialCycles == 16,
          "Scc abs.l writes condition byte after dummy read");

    endGroup();
}

static void test_movem_bus_timing() {
    beginGroup("MOVEM bus timing");

    resetCPU();
    cpu.state.d[0] = 0x00001111;
    cpu.state.d[1] = 0x00002222;
    placeAndExec4(0x48B9, 0x0003, 0x00C0, 0x0000); // MOVEM.W D0-D1,$00C00000.L
    check(write16EventCount == 2, "MOVEM.W regs-to-abs.l emits one write per word register");
    check(write16Events[0].addr == 0xC00000 && write16Events[0].val == 0x1111 &&
              write16Events[0].partialCycles == 16,
          "MOVEM.W regs-to-abs.l writes D0 after mask and address fetches");
    check(write16Events[1].addr == 0xC00002 && write16Events[1].val == 0x2222 &&
              write16Events[1].partialCycles == 20,
          "MOVEM.W regs-to-abs.l writes D1 in the next bus slot");

    resetCPU();
    cpu.state.d[0] = 0x11112222;
    cpu.state.d[1] = 0x33334444;
    placeAndExec4(0x48F9, 0x0003, 0x00C0, 0x0000); // MOVEM.L D0-D1,$00C00000.L
    check(write16EventCount == 4, "MOVEM.L regs-to-abs.l emits two word writes per long register");
    check(write16Events[0].addr == 0xC00000 && write16Events[0].val == 0x1111 &&
              write16Events[0].partialCycles == 16,
          "MOVEM.L regs-to-abs.l writes D0 high word first");
    check(write16Events[1].addr == 0xC00002 && write16Events[1].val == 0x2222 &&
              write16Events[1].partialCycles == 20,
          "MOVEM.L regs-to-abs.l writes D0 low word second");
    check(write16Events[2].addr == 0xC00004 && write16Events[2].val == 0x3333 &&
              write16Events[2].partialCycles == 24,
          "MOVEM.L regs-to-abs.l writes D1 high word in the next long slot");
    check(write16Events[3].addr == 0xC00006 && write16Events[3].val == 0x4444 &&
              write16Events[3].partialCycles == 28,
          "MOVEM.L regs-to-abs.l writes D1 low word last");

    resetCPU();
    writeWord(0xC00000, 0x1111);
    writeWord(0xC00002, 0x2222);
    writeWord(0xC00004, 0x3333);
    placeAndExec4(0x4CB9, 0x0003, 0x00C0, 0x0000); // MOVEM.W $00C00000.L,D0-D1
    check(read16EventCount == 3, "MOVEM.W abs.l-to-regs performs data reads plus dummy read");
    check(read16Events[0].addr == 0xC00000 && read16Events[0].val == 0x1111 &&
              read16Events[0].partialCycles == 16,
          "MOVEM.W abs.l-to-regs reads D0 source after mask and address fetches");
    check(read16Events[1].addr == 0xC00002 && read16Events[1].val == 0x2222 &&
              read16Events[1].partialCycles == 20,
          "MOVEM.W abs.l-to-regs reads D1 source in the next bus slot");
    check(read16Events[2].addr == 0xC00004 && read16Events[2].val == 0x3333 &&
              read16Events[2].partialCycles == 24,
          "MOVEM.W abs.l-to-regs performs documented dummy read after data reads");

    resetCPU();
    writeLong(0xC00000, 0x11112222);
    writeLong(0xC00004, 0x33334444);
    writeWord(0xC00008, 0x5555);
    placeAndExec4(0x4CF9, 0x0003, 0x00C0, 0x0000); // MOVEM.L $00C00000.L,D0-D1
    check(read16EventCount == 5, "MOVEM.L abs.l-to-regs performs data reads plus dummy read");
    check(read16Events[0].addr == 0xC00000 && read16Events[0].val == 0x1111 &&
              read16Events[0].partialCycles == 16,
          "MOVEM.L abs.l-to-regs reads D0 high word first");
    check(read16Events[1].addr == 0xC00002 && read16Events[1].val == 0x2222 &&
              read16Events[1].partialCycles == 20,
          "MOVEM.L abs.l-to-regs reads D0 low word second");
    check(read16Events[2].addr == 0xC00004 && read16Events[2].val == 0x3333 &&
              read16Events[2].partialCycles == 24,
          "MOVEM.L abs.l-to-regs reads D1 high word in the next long slot");
    check(read16Events[3].addr == 0xC00006 && read16Events[3].val == 0x4444 &&
              read16Events[3].partialCycles == 28,
          "MOVEM.L abs.l-to-regs reads D1 low word fourth");
    check(read16Events[4].addr == 0xC00008 && read16Events[4].val == 0x5555 &&
              read16Events[4].partialCycles == 32,
          "MOVEM.L abs.l-to-regs performs documented dummy read after long data reads");

    endGroup();
}

static void test_movep_bus_timing() {
    beginGroup("MOVEP bus timing");

    resetCPU();
    cpu.state.a[0] = 0xC00000;
    cpu.state.d[0] = 0x0000AABB;
    placeAndExec2(0x0188, 0x0000); // MOVEP.W D0,(0,A0)
    check(write8EventCount == 2, "MOVEP.W reg-to-mem emits two byte writes");
    check(write8Events[0].addr == 0xC00000 && write8Events[0].val == 0xAA &&
              write8Events[0].partialCycles == 8,
          "MOVEP.W reg-to-mem writes high byte after displacement fetch");
    check(write8Events[1].addr == 0xC00002 && write8Events[1].val == 0xBB &&
              write8Events[1].partialCycles == 12,
          "MOVEP.W reg-to-mem writes low byte in the next alternating slot");

    resetCPU();
    cpu.state.a[0] = 0xC00000;
    cpu.state.d[0] = 0x11223344;
    placeAndExec2(0x01C8, 0x0000); // MOVEP.L D0,(0,A0)
    check(write8EventCount == 4, "MOVEP.L reg-to-mem emits four byte writes");
    check(write8Events[0].addr == 0xC00000 && write8Events[0].val == 0x11 &&
              write8Events[0].partialCycles == 8,
          "MOVEP.L reg-to-mem writes byte 3 first");
    check(write8Events[1].addr == 0xC00002 && write8Events[1].val == 0x22 &&
              write8Events[1].partialCycles == 12,
          "MOVEP.L reg-to-mem writes byte 2 second");
    check(write8Events[2].addr == 0xC00004 && write8Events[2].val == 0x33 &&
              write8Events[2].partialCycles == 16,
          "MOVEP.L reg-to-mem writes byte 1 third");
    check(write8Events[3].addr == 0xC00006 && write8Events[3].val == 0x44 &&
              write8Events[3].partialCycles == 20,
          "MOVEP.L reg-to-mem writes byte 0 last");

    resetCPU();
    cpu.state.a[0] = 0xC00000;
    memory[0xC00000] = 0xAA;
    memory[0xC00002] = 0xBB;
    placeAndExec2(0x0108, 0x0000); // MOVEP.W (0,A0),D0
    check(read8EventCount == 2, "MOVEP.W mem-to-reg emits two byte reads");
    check(read8Events[0].addr == 0xC00000 && read8Events[0].val == 0xAA &&
              read8Events[0].partialCycles == 8,
          "MOVEP.W mem-to-reg reads high byte after displacement fetch");
    check(read8Events[1].addr == 0xC00002 && read8Events[1].val == 0xBB &&
              read8Events[1].partialCycles == 12,
          "MOVEP.W mem-to-reg reads low byte in the next alternating slot");
    check((cpu.state.d[0] & 0xFFFF) == 0xAABB, "MOVEP.W mem-to-reg assembles word");

    resetCPU();
    cpu.state.a[0] = 0xC00000;
    memory[0xC00000] = 0x11;
    memory[0xC00002] = 0x22;
    memory[0xC00004] = 0x33;
    memory[0xC00006] = 0x44;
    placeAndExec2(0x0148, 0x0000); // MOVEP.L (0,A0),D0
    check(read8EventCount == 4, "MOVEP.L mem-to-reg emits four byte reads");
    check(read8Events[0].addr == 0xC00000 && read8Events[0].val == 0x11 &&
              read8Events[0].partialCycles == 8,
          "MOVEP.L mem-to-reg reads byte 3 first");
    check(read8Events[1].addr == 0xC00002 && read8Events[1].val == 0x22 &&
              read8Events[1].partialCycles == 12,
          "MOVEP.L mem-to-reg reads byte 2 second");
    check(read8Events[2].addr == 0xC00004 && read8Events[2].val == 0x33 &&
              read8Events[2].partialCycles == 16,
          "MOVEP.L mem-to-reg reads byte 1 third");
    check(read8Events[3].addr == 0xC00006 && read8Events[3].val == 0x44 &&
              read8Events[3].partialCycles == 20,
          "MOVEP.L mem-to-reg reads byte 0 last");
    check(cpu.state.d[0] == 0x11223344, "MOVEP.L mem-to-reg assembles long");

    endGroup();
}

// --- Test suites ---

static void test_moveq() {
    beginGroup("MOVEQ");

    resetCPU();
    // MOVEQ #42, D0  => 0x7000 | (0 << 9) | 42 = 0x702A
    int cycles = placeAndExec(0x702A);
    check(cpu.state.d[0] == 42, "MOVEQ #42,D0 value");
    check(cycles == 4, "MOVEQ #42,D0 cycles");
    check(!(cpu.state.sr & FLAG_N), "MOVEQ #42,D0 N flag clear");
    check(!(cpu.state.sr & FLAG_Z), "MOVEQ #42,D0 Z flag clear");

    // MOVEQ #0, D1
    placeAndExec(0x7200); // D1, data=0
    check(cpu.state.d[1] == 0, "MOVEQ #0,D1 value");
    check(cpu.state.sr & FLAG_Z, "MOVEQ #0,D1 Z flag set");

    // MOVEQ #-1 (0xFF), D2
    placeAndExec(0x74FF); // D2, data=0xFF = -1
    check(cpu.state.d[2] == 0xFFFFFFFF, "MOVEQ #-1,D2 sign extension");
    check(cpu.state.sr & FLAG_N, "MOVEQ #-1,D2 N flag set");

    endGroup();
}

static void test_add() {
    beginGroup("ADD");

    resetCPU();
    cpu.state.d[0] = 0x00000010;
    cpu.state.d[1] = 0x00000020;
    // ADD.L D1,D0 => 0xD081 (D0 = D0 + D1 direction: to reg, src=D1)
    // ADD.L Dn,Dn: opcode = 1101 rrr0 10mm mrrr
    // D0 = D0 + D1: 1101 000 0 10 000 001 = 0xD081
    placeAndExec(0xD081);
    check(cpu.state.d[0] == 0x30, "ADD.L D1,D0 result");

    // ADD.W with carry
    resetCPU();
    cpu.state.d[0] = 0xFFF0;
    cpu.state.d[1] = 0x0020;
    // ADD.W D1,D0: 1101 000 0 01 000 001 = 0xD041
    placeAndExec(0xD041);
    check((cpu.state.d[0] & 0xFFFF) == 0x0010, "ADD.W with carry result");
    check(cpu.state.sr & FLAG_C, "ADD.W with carry C flag");
    check(cpu.state.sr & FLAG_X, "ADD.W with carry X flag");

    // ADD.B overflow
    resetCPU();
    cpu.state.d[0] = 0x7F;
    cpu.state.d[1] = 0x01;
    // ADD.B D1,D0: 1101 000 0 00 000 001 = 0xD001
    placeAndExec(0xD001);
    check((cpu.state.d[0] & 0xFF) == 0x80, "ADD.B overflow result");
    check(cpu.state.sr & FLAG_V, "ADD.B overflow V flag");
    check(cpu.state.sr & FLAG_N, "ADD.B overflow N flag");

    endGroup();
}

static void test_sub() {
    beginGroup("SUB");

    resetCPU();
    cpu.state.d[0] = 0x00000030;
    cpu.state.d[1] = 0x00000010;
    // SUB.L D1,D0: 1001 000 0 10 000 001 = 0x9081
    placeAndExec(0x9081);
    check(cpu.state.d[0] == 0x20, "SUB.L D1,D0 result");

    // SUB with borrow
    resetCPU();
    cpu.state.d[0] = 0x0010;
    cpu.state.d[1] = 0x0020;
    // SUB.W D1,D0: 1001 000 0 01 000 001 = 0x9041
    placeAndExec(0x9041);
    check((cpu.state.d[0] & 0xFFFF) == 0xFFF0, "SUB.W borrow result");
    check(cpu.state.sr & FLAG_C, "SUB.W borrow C flag");
    check(cpu.state.sr & FLAG_N, "SUB.W borrow N flag");

    endGroup();
}

static void test_mulu() {
    beginGroup("MULU");

    // MULU Dn,Dn: 1100 rrr 011 000 rrr
    // MULU D1,D0: 1100 000 011 000 001 = 0xC0C1

    // Basic multiply
    resetCPU();
    cpu.state.d[0] = 100;
    cpu.state.d[1] = 200;
    int cycles = placeAndExec(0xC0C1);
    check(cpu.state.d[0] == 20000, "MULU 100*200 result");
    // 200 in binary = 0b11001000, popcount = 3
    // Expected: 38 + 2*3 + 0 (Dn EA) = 44
    check(cycles == 44, "MULU 100*200 cycles (38+2*popcount)");

    // Multiply by zero
    resetCPU();
    cpu.state.d[0] = 12345;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xC0C1);
    check(cpu.state.d[0] == 0, "MULU *0 result");
    check(cpu.state.sr & FLAG_Z, "MULU *0 Z flag");
    // popcount(0) = 0, cycles = 38
    check(cycles == 38, "MULU *0 cycles");

    // Max multiply
    resetCPU();
    cpu.state.d[0] = 0xFFFF;
    cpu.state.d[1] = 0xFFFF;
    cycles = placeAndExec(0xC0C1);
    check(cpu.state.d[0] == 0xFFFE0001, "MULU 0xFFFF*0xFFFF result");
    // popcount(0xFFFF) = 16, cycles = 38 + 32 = 70
    check(cycles == 70, "MULU max cycles (38+2*16=70)");

    endGroup();
}

static void test_muls() {
    beginGroup("MULS");

    // MULS Dn,Dn: 1100 rrr 111 000 rrr
    // MULS D1,D0: 1100 000 111 000 001 = 0xC1C1

    resetCPU();
    cpu.state.d[0] = 100;
    cpu.state.d[1] = static_cast<u32>(static_cast<s16>(-50)) & 0xFFFF;
    int cycles = placeAndExec(0xC1C1);
    check(cpu.state.d[0] == static_cast<u32>(-5000), "MULS 100*-50 result");
    check(cpu.state.sr & FLAG_N, "MULS negative N flag");

    // MULS with zero source
    resetCPU();
    cpu.state.d[0] = 0x1234;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xC1C1);
    check(cpu.state.d[0] == 0, "MULS *0 result");
    // src=0, transitions = 0^0 = 0, popcount=0, cycles = 38
    check(cycles == 38, "MULS *0 cycles");

    // MULS with alternating bits (max transitions)
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0x5555; // 0101010101010101 - many transitions
    cycles = placeAndExec(0xC1C1);
    check(cpu.state.d[0] == 0x5555, "MULS 1*0x5555 result");
    // 0x5555 = 0101010101010101, transitions: v^(v<<1) = 0101010101010101 ^ 1010101010101010 = 1111111111111111 & 0xFFFF
    // popcount = 16, cycles = 38 + 32 = 70
    // Hmm wait: 0x5555 = 0b0101_0101_0101_0101
    // v<<1 = 0b1010_1010_1010_1010
    // v ^ (v<<1) = 0b1111_1111_1111_1111
    // popcount = 16
    check(cycles == 70, "MULS alternating cycles (38+2*16=70)");

    endGroup();
}

static void test_divu() {
    beginGroup("DIVU");

    // DIVU Dn,Dn: 1000 rrr 011 000 rrr
    // DIVU D1,D0: 1000 000 011 000 001 = 0x80C1

    // Basic division
    resetCPU();
    cpu.state.d[0] = 1000;
    cpu.state.d[1] = 10;
    int cycles = placeAndExec(0x80C1);
    check((cpu.state.d[0] & 0xFFFF) == 100, "DIVU quotient");
    check((cpu.state.d[0] >> 16) == 0, "DIVU remainder");
    check(!(cpu.state.sr & FLAG_V), "DIVU no overflow");

    // Division with remainder
    resetCPU();
    cpu.state.d[0] = 103;
    cpu.state.d[1] = 10;
    placeAndExec(0x80C1);
    check((cpu.state.d[0] & 0xFFFF) == 10, "DIVU 103/10 quotient");
    check((cpu.state.d[0] >> 16) == 3, "DIVU 103/10 remainder");

    // Division overflow (quotient > 0xFFFF)
    resetCPU();
    cpu.state.d[0] = 0x10000;
    cpu.state.d[1] = 1;
    cycles = placeAndExec(0x80C1);
    check(cpu.state.sr & FLAG_V, "DIVU overflow V flag");

    // Cycle timing follows Yacht's operand-dependent DIVU loop, not a
    // quotient-popcount approximation.
    resetCPU();
    cpu.state.d[0] = 0; // dividend=0
    cpu.state.d[1] = 1; // divisor=1
    cycles = placeAndExec(0x80C1);
    check((cpu.state.d[0] & 0xFFFF) == 0, "DIVU 0/1 quotient");
    check(cycles == 136, "DIVU 0/1 cycles (Yacht)");

    resetCPU();
    cpu.state.d[0] = 0xFFFF; // dividend=65535
    cpu.state.d[1] = 1;      // divisor=1
    cycles = placeAndExec(0x80C1);
    check(cycles == 106, "DIVU 0xFFFF/1 cycles (Yacht)");

    // Cotton gameplay line-183 operand: DIVU.W #7,D0 with D0=0x000323FC.
    resetCPU();
    cpu.state.d[0] = 0x000323FC;
    cycles = placeAndExec2(0x80FC, 0x0007);
    check((cpu.state.d[0] & 0xFFFF) == 29402, "DIVU Cotton #7 quotient");
    check((cpu.state.d[0] >> 16) == 6, "DIVU Cotton #7 remainder");
    check(cycles == 122, "DIVU Cotton #7 cycles (Yacht core + immediate EA)");

    endGroup();
}

static void test_divs() {
    beginGroup("DIVS");

    // DIVS Dn,Dn: 1000 rrr 111 000 rrr
    // DIVS D1,D0: 1000 000 111 000 001 = 0x81C1

    // Basic signed division
    resetCPU();
    cpu.state.d[0] = static_cast<u32>(static_cast<s32>(-1000));
    cpu.state.d[1] = 10;
    int cycles = placeAndExec(0x81C1);
    check(static_cast<s16>(cpu.state.d[0] & 0xFFFF) == -100, "DIVS -1000/10 quotient");
    check(static_cast<s16>(cpu.state.d[0] >> 16) == 0, "DIVS -1000/10 remainder");
    check(cycles == 150, "DIVS -1000/10 cycles (Yacht)");

    // Overflow
    resetCPU();
    cpu.state.d[0] = 0x80000000; // large negative
    cpu.state.d[1] = 1;
    placeAndExec(0x81C1);
    check(cpu.state.sr & FLAG_V, "DIVS overflow V flag");

    // Division of positive by negative
    resetCPU();
    cpu.state.d[0] = 100;
    cpu.state.d[1] = static_cast<u32>(static_cast<s16>(-10)) & 0xFFFF;
    cycles = placeAndExec(0x81C1);
    check(static_cast<s16>(cpu.state.d[0] & 0xFFFF) == -10, "DIVS 100/-10 quotient");
    check(static_cast<s16>(cpu.state.d[0] >> 16) == 0, "DIVS 100/-10 remainder");
    check(cycles == 148, "DIVS 100/-10 cycles (Yacht)");

    endGroup();
}

static void test_cmp() {
    beginGroup("CMP");

    resetCPU();
    cpu.state.d[0] = 0x42;
    cpu.state.d[1] = 0x42;
    cpu.state.sr |= FLAG_X;
    // CMP.L D1,D0: 1011 000 010 000 001 = 0xB081
    placeAndExec(0xB081);
    check(cpu.state.sr & FLAG_Z, "CMP equal Z flag");
    check(!(cpu.state.sr & FLAG_N), "CMP equal N flag clear");
    check(!(cpu.state.sr & FLAG_C), "CMP equal C flag clear");
    check(cpu.state.sr & FLAG_X, "CMP preserves X flag");

    resetCPU();
    cpu.state.d[0] = 0x10;
    cpu.state.d[1] = 0x20;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xB081);
    check(!(cpu.state.sr & FLAG_Z), "CMP less Z clear");
    check(cpu.state.sr & FLAG_C, "CMP less C flag (borrow)");
    check(!(cpu.state.sr & FLAG_X), "CMP borrow still preserves X flag");

    endGroup();
}

static void test_neg() {
    beginGroup("NEG");

    resetCPU();
    cpu.state.d[0] = 0x0001;
    // NEG.W D0: 0100 0100 01 000 000 = 0x4440
    placeAndExec(0x4440);
    check((cpu.state.d[0] & 0xFFFF) == 0xFFFF, "NEG.W 1 result");
    check(cpu.state.sr & FLAG_N, "NEG.W 1 N flag");
    check(cpu.state.sr & FLAG_C, "NEG.W non-zero C flag");

    resetCPU();
    cpu.state.d[0] = 0x0000;
    placeAndExec(0x4440);
    check((cpu.state.d[0] & 0xFFFF) == 0, "NEG.W 0 result");
    check(cpu.state.sr & FLAG_Z, "NEG.W 0 Z flag");
    check(!(cpu.state.sr & FLAG_C), "NEG.W 0 C flag clear");

    endGroup();
}

static void test_clr() {
    beginGroup("CLR");

    resetCPU();
    cpu.state.d[0] = 0xDEADBEEF;
    // CLR.L D0: 0100 0010 10 000 000 = 0x4280
    placeAndExec(0x4280);
    check(cpu.state.d[0] == 0, "CLR.L D0 result");
    check(cpu.state.sr & FLAG_Z, "CLR.L Z flag");
    check(!(cpu.state.sr & FLAG_N), "CLR.L N flag clear");

    endGroup();
}

static void test_and() {
    beginGroup("AND");

    resetCPU();
    cpu.state.d[0] = 0xFF00FF00;
    cpu.state.d[1] = 0x12345678;
    // AND.L D1,D0: 1100 000 010 000 001 = 0xC081
    placeAndExec(0xC081);
    check(cpu.state.d[0] == 0x12005600, "AND.L result");

    endGroup();
}

static void test_or() {
    beginGroup("OR");

    resetCPU();
    cpu.state.d[0] = 0x00FF0000;
    cpu.state.d[1] = 0x0000FF00;
    // OR.L D1,D0: 1000 000 010 000 001 = 0x8081
    placeAndExec(0x8081);
    check(cpu.state.d[0] == 0x00FFFF00, "OR.L result");

    endGroup();
}

static void test_eor() {
    beginGroup("EOR");

    resetCPU();
    cpu.state.d[0] = 0xFFFFFFFF;
    cpu.state.d[1] = 0x12345678;
    // EOR.L D0,D1: 1011 000 110 000 001 = 0xB181
    placeAndExec(0xB181);
    check(cpu.state.d[1] == 0xEDCBA987, "EOR.L result");

    endGroup();
}

static void test_not() {
    beginGroup("NOT");

    resetCPU();
    cpu.state.d[0] = 0x00FF00FF;
    // NOT.L D0: 0100 0110 10 000 000 = 0x4680
    placeAndExec(0x4680);
    check(cpu.state.d[0] == 0xFF00FF00, "NOT.L result");

    endGroup();
}

static void test_tst() {
    beginGroup("TST");

    resetCPU();
    cpu.state.d[0] = 0;
    // TST.W D0: 0100 1010 01 000 000 = 0x4A40
    placeAndExec(0x4A40);
    check(cpu.state.sr & FLAG_Z, "TST.W zero Z flag");
    check(!(cpu.state.sr & FLAG_N), "TST.W zero N flag clear");

    resetCPU();
    cpu.state.d[0] = 0x8000;
    placeAndExec(0x4A40);
    check(!(cpu.state.sr & FLAG_Z), "TST.W negative Z clear");
    check(cpu.state.sr & FLAG_N, "TST.W negative N flag");

    endGroup();
}

static void test_ext() {
    beginGroup("EXT");

    resetCPU();
    cpu.state.d[0] = 0x000000FF; // byte -1
    // EXT.W D0: 0100 1000 10 000 000 = 0x4880
    placeAndExec(0x4880);
    check((cpu.state.d[0] & 0xFFFF) == 0xFFFF, "EXT.W sign extend byte -1 to word");

    resetCPU();
    cpu.state.d[0] = 0x0000FFFF; // word -1
    // EXT.L D0: 0100 1000 11 000 000 = 0x48C0
    placeAndExec(0x48C0);
    check(cpu.state.d[0] == 0xFFFFFFFF, "EXT.L sign extend word -1 to long");

    resetCPU();
    cpu.state.d[0] = 0x0000007F; // byte +127
    placeAndExec(0x4880);
    check((cpu.state.d[0] & 0xFFFF) == 0x007F, "EXT.W sign extend byte +127");

    endGroup();
}

static void test_swap() {
    beginGroup("SWAP");

    resetCPU();
    cpu.state.d[0] = 0x12345678;
    // SWAP D0: 0100 1000 01 000 000 = 0x4840
    placeAndExec(0x4840);
    check(cpu.state.d[0] == 0x56781234, "SWAP D0 result");

    endGroup();
}

static void test_asl_asr() {
    beginGroup("ASL/ASR");

    // ASL register: count=2, size=word, left, D0
    // 1110 ccc1 ss1 00rrr  (imm count, left)
    // count=2, size=word(01), left(1), reg D0
    // 1110 010 1 01 1 00 000 = 0xE540
    resetCPU();
    cpu.state.d[0] = 0x0001;
    int cycles = placeAndExec(0xE540);
    check((cpu.state.d[0] & 0xFFFF) == 0x0004, "ASL.W #2,D0 result");
    check(cycles == 10, "ASL.W #2 cycles (6+2*2=10)");

    // ASR register: count=1, size=byte, right, D0
    // 1110 ccc0 ss0 00rrr (imm count, right)
    // count=1, size=byte(00), D0
    // 1110 001 0 00 0 00 000 = 0xE200
    resetCPU();
    cpu.state.d[0] = 0x80; // -128 in byte
    cycles = placeAndExec(0xE200);
    check((cpu.state.d[0] & 0xFF) == 0xC0, "ASR.B #1,D0 result (-128>>1 = -64)");
    check(cycles == 8, "ASR.B #1 cycles (6+2=8)");

    // ASL.L: size=long, should use base 8
    // count=1, size=long(10), left, D0
    // 1110 001 1 10 1 00 000 = 0xE380
    resetCPU();
    cpu.state.d[0] = 0x00000001;
    cycles = placeAndExec(0xE380);
    check(cpu.state.d[0] == 0x00000002, "ASL.L #1,D0 result");
    check(cycles == 10, "ASL.L #1 cycles (8+2*1=10)");

    // ASR.L with count=3
    // 1110 011 0 10 0 00 000 = 0xEE80... wait let me recalculate
    // count=3, right, long, D0
    // 1110 011 0 10 0 00 000 = 0xE680
    resetCPU();
    cpu.state.d[0] = 0x80000000;
    cycles = placeAndExec(0xE680);
    check(cpu.state.d[0] == 0xF0000000, "ASR.L #3,D0 result");
    check(cycles == 14, "ASR.L #3 cycles (8+2*3=14)");

    endGroup();
}

static void test_lsl_lsr() {
    beginGroup("LSL/LSR");

    // LSL.W #1,D0: 1110 001 1 01 0 01 000 = 0xE348
    resetCPU();
    cpu.state.d[0] = 0x8001;
    int cycles = placeAndExec(0xE348);
    check((cpu.state.d[0] & 0xFFFF) == 0x0002, "LSL.W #1,D0 result");
    check(cpu.state.sr & FLAG_C, "LSL.W MSB shifted to C");
    check(cycles == 8, "LSL.W #1 cycles (6+2=8)");

    // LSR.W #1,D0: 1110 001 0 01 0 01 000 = 0xE248
    resetCPU();
    cpu.state.d[0] = 0x8001;
    cycles = placeAndExec(0xE248);
    check((cpu.state.d[0] & 0xFFFF) == 0x4000, "LSR.W #1,D0 result");
    check(cpu.state.sr & FLAG_C, "LSR.W LSB shifted to C");

    // LSL.L: base timing should be 8
    // LSL.L #2,D0: 1110 010 1 10 0 01 000 = 0xE588
    resetCPU();
    cpu.state.d[0] = 0x00000001;
    cycles = placeAndExec(0xE588);
    check(cpu.state.d[0] == 0x00000004, "LSL.L #2,D0 result");
    check(cycles == 12, "LSL.L #2 cycles (8+2*2=12)");

    // Shift count=0 (from register, register contains 0)
    // LSL.W D1,D0 (register count): 1110 001 1 01 1 01 000 = 0xE368
    resetCPU();
    cpu.state.d[0] = 0x1234;
    cpu.state.d[1] = 0; // count=0
    cycles = placeAndExec(0xE368);
    check((cpu.state.d[0] & 0xFFFF) == 0x1234, "LSL.W D1(=0),D0 unchanged");
    check(!(cpu.state.sr & FLAG_C), "LSL count=0 C cleared");
    check(cycles == 6, "LSL.W count=0 cycles (6+0=6)");

    endGroup();
}

static void test_rol_ror() {
    beginGroup("ROL/ROR");

    // ROL.W #1,D0: 1110 001 1 01 0 11 000 = 0xE358
    resetCPU();
    cpu.state.d[0] = 0x8001;
    int cycles = placeAndExec(0xE358);
    check((cpu.state.d[0] & 0xFFFF) == 0x0003, "ROL.W #1 result");
    check(cycles == 8, "ROL.W #1 cycles (6+2=8)");

    // ROR.W #1,D0: 1110 001 0 01 0 11 000 = 0xE258
    resetCPU();
    cpu.state.d[0] = 0x0001;
    cycles = placeAndExec(0xE258);
    check((cpu.state.d[0] & 0xFFFF) == 0x8000, "ROR.W #1 result");
    check(cycles == 8, "ROR.W #1 cycles (6+2=8)");

    // ROL.L #1 base timing = 8
    // ROL.L #1,D0: 1110 001 1 10 0 11 000 = 0xE398
    resetCPU();
    cpu.state.d[0] = 0x80000001;
    cycles = placeAndExec(0xE398);
    check(cpu.state.d[0] == 0x00000003, "ROL.L #1 result");
    check(cycles == 10, "ROL.L #1 cycles (8+2=10)");

    endGroup();
}

static void test_roxl_roxr() {
    beginGroup("ROXL/ROXR");

    // ROXL.W #1,D0: 1110 001 1 01 0 10 000 = 0xE350
    resetCPU();
    cpu.state.d[0] = 0x8000;
    cpu.state.sr |= FLAG_X; // X=1
    int cycles = placeAndExec(0xE350);
    check((cpu.state.d[0] & 0xFFFF) == 0x0001, "ROXL.W #1 result (X rotated in)");
    check(cpu.state.sr & FLAG_X, "ROXL.W X flag (old MSB)");
    check(cycles == 8, "ROXL.W #1 cycles (6+2=8)");

    // ROXL.L #1 base timing = 8
    // ROXL.L #1,D0: 1110 001 1 10 0 10 000 = 0xE390
    resetCPU();
    cpu.state.d[0] = 0x00000001;
    cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0xE390);
    check(cpu.state.d[0] == 0x00000002, "ROXL.L #1 result");
    check(cycles == 10, "ROXL.L #1 cycles (8+2=10)");

    endGroup();
}

static void test_addq_subq() {
    beginGroup("ADDQ/SUBQ");

    resetCPU();
    cpu.state.d[0] = 10;
    // ADDQ.L #3,D0: 0101 011 0 10 000 000 = 0x5680
    placeAndExec(0x5680);
    check(cpu.state.d[0] == 13, "ADDQ.L #3,D0 result");

    resetCPU();
    cpu.state.d[0] = 10;
    // SUBQ.L #3,D0: 0101 011 1 10 000 000 = 0x5780
    placeAndExec(0x5780);
    check(cpu.state.d[0] == 7, "SUBQ.L #3,D0 result");

    // ADDQ #8 (encoded as 0): 0101 000 0 10 000 000 = 0x5080
    resetCPU();
    cpu.state.d[0] = 0;
    placeAndExec(0x5080);
    check(cpu.state.d[0] == 8, "ADDQ.L #8 (enc=0) result");

    endGroup();
}

static void test_bra_bcc() {
    beginGroup("BRA/Bcc");

    // BRA short (+4): 0110 0000 0000 0100 = 0x6004
    resetCPU();
    u32 startPC = cpu.state.pc;
    placeAndExec(0x6004);
    check(cpu.state.pc == startPC + 2 + 4, "BRA.S +4 target");

    // BEQ taken (Z=1): 0110 0111 0000 0004 = 0x6704
    resetCPU();
    startPC = cpu.state.pc;
    cpu.state.sr |= FLAG_Z;
    placeAndExec(0x6704);
    check(cpu.state.pc == startPC + 2 + 4, "BEQ.S taken");

    // BEQ not taken (Z=0)
    resetCPU();
    startPC = cpu.state.pc;
    cpu.state.sr &= ~FLAG_Z;
    placeAndExec(0x6704);
    check(cpu.state.pc == startPC + 2, "BEQ.S not taken");

    // BNE taken (Z=0): 0110 0110 0000 0004 = 0x6604
    resetCPU();
    startPC = cpu.state.pc;
    cpu.state.sr &= ~FLAG_Z;
    placeAndExec(0x6604);
    check(cpu.state.pc == startPC + 2 + 4, "BNE.S taken");

    endGroup();
}

static void test_dbcc() {
    beginGroup("DBcc");

    // DBRA (DBF) D0: 0101 0001 11 001 000 = 0x51C8, displacement word follows
    // Count down from 3 to -1 (4 iterations)
    resetCPU();
    cpu.state.d[0] = 3;
    u32 loopPC = cpu.state.pc;
    // Place DBRA D0, -4 (loop back to self)
    writeWord(loopPC, 0x51C8);
    writeWord(loopPC + 2, 0xFFFE); // displacement = -2 (relative to ext word addr)

    int totalCycles = 0;
    for (int i = 0; i < 4; i++) {
        cpu.state.pc = loopPC;
        totalCycles += cpu.execute();
    }
    check(cpu.state.d[0] == 0xFFFF, "DBRA D0 counted down to -1 (0xFFFF)");

    // DBEQ with condition true (Z set) - should NOT decrement
    resetCPU();
    cpu.state.d[0] = 5;
    cpu.state.sr |= FLAG_Z;
    writeWord(cpu.state.pc, 0x57C8); // DBEQ D0
    writeWord(cpu.state.pc + 2, 0x0004);
    u32 pc_before = cpu.state.pc;
    cpu.execute();
    check((cpu.state.d[0] & 0xFFFF) == 5, "DBEQ condition true - count unchanged");
    check(cpu.state.pc == pc_before + 4, "DBEQ condition true - falls through");

    endGroup();
}

static void test_bsr_rts() {
    beginGroup("BSR/RTS");

    resetCPU();
    u32 startPC = cpu.state.pc;
    u32 startSP = cpu.state.a[7];
    // BSR.S +6: 0110 0001 0000 0110 = 0x6106
    // After BSR: PC = startPC+2+6, stack has return addr = startPC+2
    placeAndExec(0x6106);
    check(cpu.state.pc == startPC + 2 + 6, "BSR.S target");
    check(cpu.state.a[7] == startSP - 4, "BSR.S pushed return address");

    // Now place RTS at current PC and execute
    // RTS: 0100 1110 0111 0101 = 0x4E75
    placeAndExec(0x4E75);
    check(cpu.state.pc == startPC + 2, "RTS returned to correct address");
    check(cpu.state.a[7] == startSP, "RTS restored stack");

    endGroup();
}

static void test_jmp_jsr() {
    beginGroup("JMP/JSR");

    // JMP (A0): 0100 1110 11 010 000 = 0x4ED0
    resetCPU();
    cpu.state.a[0] = 0x3000;
    placeAndExec(0x4ED0);
    check(cpu.state.pc == 0x3000, "JMP (A0) target");

    // JSR (A0): 0100 1110 10 010 000 = 0x4E90
    resetCPU();
    u32 startSP = cpu.state.a[7];
    cpu.state.a[0] = 0x3000;
    u32 retAddr = cpu.state.pc + 2;
    placeAndExec(0x4E90);
    check(cpu.state.pc == 0x3000, "JSR (A0) target");
    check(cpu.state.a[7] == startSP - 4, "JSR pushed return address");

    endGroup();
}

static void test_lea_pea() {
    beginGroup("LEA/PEA");

    // LEA (A0),A1: 0100 001 111 010 000 = 0x43D0
    resetCPU();
    cpu.state.a[0] = 0x5000;
    placeAndExec(0x43D0);
    check(cpu.state.a[1] == 0x5000, "LEA (A0),A1 result");

    // PEA (A0): 0100 1000 01 010 000 = 0x4850
    resetCPU();
    cpu.state.a[0] = 0x5000;
    u32 startSP = cpu.state.a[7];
    placeAndExec(0x4850);
    check(cpu.state.a[7] == startSP - 4, "PEA (A0) decremented SP");

    endGroup();
}

static void test_exg() {
    beginGroup("EXG");

    // EXG D0,D1: 1100 000 1 01000 001 = 0xC141
    resetCPU();
    cpu.state.d[0] = 0xAAAA;
    cpu.state.d[1] = 0xBBBB;
    placeAndExec(0xC141);
    check(cpu.state.d[0] == 0xBBBB, "EXG D0,D1 - D0");
    check(cpu.state.d[1] == 0xAAAA, "EXG D0,D1 - D1");

    // EXG A0,A1: 1100 000 1 01001 001 = 0xC149
    resetCPU();
    cpu.state.a[0] = 0x1111;
    cpu.state.a[1] = 0x2222;
    placeAndExec(0xC149);
    check(cpu.state.a[0] == 0x2222, "EXG A0,A1 - A0");
    check(cpu.state.a[1] == 0x1111, "EXG A0,A1 - A1");

    endGroup();
}

static void test_btst() {
    beginGroup("BTST/BCHG/BCLR/BSET");

    // BTST #0,D0 (static): 0000 1000 00 000 000 = 0x0800, ext word = bit#
    resetCPU();
    cpu.state.d[0] = 0x01;
    placeAndExec2(0x0800, 0x0000); // bit 0
    check(!(cpu.state.sr & FLAG_Z), "BTST #0 on bit set: Z clear");

    resetCPU();
    cpu.state.d[0] = 0x00;
    placeAndExec2(0x0800, 0x0000); // bit 0
    check(cpu.state.sr & FLAG_Z, "BTST #0 on bit clear: Z set");

    // BSET #7,D0: 0000 1000 11 000 000 = 0x08C0, ext = 7
    resetCPU();
    cpu.state.d[0] = 0x00;
    placeAndExec2(0x08C0, 0x0007);
    check(cpu.state.d[0] & 0x80, "BSET #7 result");
    check(cpu.state.sr & FLAG_Z, "BSET #7 Z (was clear before)");

    // BCLR #7,D0: 0000 1000 10 000 000 = 0x0880, ext = 7
    resetCPU();
    cpu.state.d[0] = 0xFF;
    placeAndExec2(0x0880, 0x0007);
    check(!(cpu.state.d[0] & 0x80), "BCLR #7 result");
    check(!(cpu.state.sr & FLAG_Z), "BCLR #7 Z (was set before)");

    endGroup();
}

static void test_move_sr() {
    beginGroup("MOVE to/from SR/CCR");

    // MOVE #$001F,CCR: 0100 0100 11 111 100 = 0x44FC, ext = 0x001F
    resetCPU();
    cpu.state.sr = 0x2700; // supervisor
    placeAndExec2(0x44FC, 0x001F);
    check((cpu.state.sr & 0xFF) == 0x1F, "MOVE #$1F,CCR sets all condition flags");
    check(cpu.state.sr & FLAG_S, "MOVE to CCR preserves S bit");

    // MOVE SR,D0: 0100 0000 11 000 000 = 0x40C0
    resetCPU();
    cpu.state.sr = 0x2704;
    placeAndExec(0x40C0);
    check((cpu.state.d[0] & 0xFFFF) == 0x2704, "MOVE SR,D0 result");

    endGroup();
}

static void test_trap() {
    beginGroup("TRAP");

    // TRAP #0: 0100 1110 0100 0000 = 0x4E40
    resetCPU();
    // Set up trap vector 32 (TRAP #0)
    writeLong(32 * 4, 0x00004000);
    placeAndExec(0x4E40);
    check(cpu.state.pc == 0x4000, "TRAP #0 vectored to handler");

    endGroup();
}

static void test_nop() {
    beginGroup("NOP");

    resetCPU();
    u32 startPC = cpu.state.pc;
    // NOP: 0100 1110 0111 0001 = 0x4E71
    int cycles = placeAndExec(0x4E71);
    check(cpu.state.pc == startPC + 2, "NOP advances PC");
    check(cycles == 4, "NOP cycles");

    endGroup();
}

static void test_addi_subi_cmpi() {
    beginGroup("ADDI/SUBI/CMPI");

    // ADDI.W #$1234,D0: 0000 0110 01 000 000 = 0x0640, ext = 0x1234
    resetCPU();
    cpu.state.d[0] = 0x1000;
    placeAndExec2(0x0640, 0x1234);
    check((cpu.state.d[0] & 0xFFFF) == 0x2234, "ADDI.W #$1234,D0 result");

    // SUBI.W #$0100,D0: 0000 0100 01 000 000 = 0x0440, ext = 0x0100
    resetCPU();
    cpu.state.d[0] = 0x0300;
    placeAndExec2(0x0440, 0x0100);
    check((cpu.state.d[0] & 0xFFFF) == 0x0200, "SUBI.W #$100,D0 result");

    // CMPI.W #$42,D0: 0000 1100 01 000 000 = 0x0C40, ext = 0x0042
    resetCPU();
    cpu.state.d[0] = 0x0042;
    cpu.state.sr |= FLAG_X;
    placeAndExec2(0x0C40, 0x0042);
    check(cpu.state.sr & FLAG_Z, "CMPI.W equal Z flag");
    check(cpu.state.sr & FLAG_X, "CMPI preserves X flag");

    endGroup();
}

static void test_ori_andi_eori() {
    beginGroup("ORI/ANDI/EORI");

    // ORI.W #$FF00,D0: 0000 0000 01 000 000 = 0x0040, ext = 0xFF00
    resetCPU();
    cpu.state.d[0] = 0x00FF;
    placeAndExec2(0x0040, 0xFF00);
    check((cpu.state.d[0] & 0xFFFF) == 0xFFFF, "ORI.W result");

    // ANDI.W #$00FF,D0: 0000 0010 01 000 000 = 0x0240, ext = 0x00FF
    resetCPU();
    cpu.state.d[0] = 0x1234;
    placeAndExec2(0x0240, 0x00FF);
    check((cpu.state.d[0] & 0xFFFF) == 0x0034, "ANDI.W result");

    // EORI.W #$FFFF,D0: 0000 1010 01 000 000 = 0x0A40, ext = 0xFFFF
    resetCPU();
    cpu.state.d[0] = 0x1234;
    placeAndExec2(0x0A40, 0xFFFF);
    check((cpu.state.d[0] & 0xFFFF) == 0xEDCB, "EORI.W result");

    endGroup();
}

static void test_move() {
    beginGroup("MOVE");

    // MOVE.L D0,D1: 0010 001 000 000 000 = 0x2200
    resetCPU();
    cpu.state.d[0] = 0xDEADBEEF;
    placeAndExec(0x2200);
    check(cpu.state.d[1] == 0xDEADBEEF, "MOVE.L D0,D1 result");

    // MOVE.W D0,D1: 0011 001 000 000 000 = 0x3200
    resetCPU();
    cpu.state.d[0] = 0x1234;
    cpu.state.d[1] = 0xFFFF0000;
    placeAndExec(0x3200);
    check(cpu.state.d[1] == 0xFFFF1234, "MOVE.W D0,D1 preserves upper");

    // MOVE.B D0,D1: 0001 001 000 000 000 = 0x1200
    resetCPU();
    cpu.state.d[0] = 0xAB;
    cpu.state.d[1] = 0xFFFFFF00;
    placeAndExec(0x1200);
    check(cpu.state.d[1] == 0xFFFFFFAB, "MOVE.B D0,D1 preserves upper");

    // MOVE.W #imm,D0: 0011 000 000 111 100 = 0x303C, ext = value
    resetCPU();
    placeAndExec2(0x303C, 0x5678);
    check((cpu.state.d[0] & 0xFFFF) == 0x5678, "MOVE.W #$5678,D0 result");

    // MOVE.B (A0)+,(A1)+: PRM/Yacht table lists 12 cycles for byte memory-to-postincrement.
    resetCPU();
    cpu.state.a[0] = 0x4000;
    cpu.state.a[1] = 0x5000;
    memory[0x4000] = 0x5A;
    int cycles = placeAndExec(0x12D8);
    check(memory[0x5000] == 0x5A, "MOVE.B (A0)+,(A1)+ writes byte");
    check(cpu.state.a[0] == 0x4001, "MOVE.B (A0)+,(A1)+ increments source");
    check(cpu.state.a[1] == 0x5001, "MOVE.B (A0)+,(A1)+ increments destination");
    check(cycles == 12, "MOVE.B (A0)+,(A1)+ cycles");

    endGroup();
}

static void test_movea() {
    beginGroup("MOVEA");

    // MOVEA.W D0,A0: 0011 000 001 000 000 = 0x3040
    resetCPU();
    cpu.state.d[0] = 0x8000; // negative word
    placeAndExec(0x3040);
    check(cpu.state.a[0] == 0xFFFF8000, "MOVEA.W sign extends");

    // MOVEA.L D0,A0: 0010 000 001 000 000 = 0x2040
    resetCPU();
    cpu.state.d[0] = 0x12345678;
    placeAndExec(0x2040);
    check(cpu.state.a[0] == 0x12345678, "MOVEA.L direct copy");

    endGroup();
}

static void test_abcd_sbcd() {
    beginGroup("ABCD/SBCD");

    // ABCD D1,D0: 1100 000 1 00000 001 = 0xC101
    resetCPU();
    cpu.state.d[0] = 0x15;
    cpu.state.d[1] = 0x27;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xC101);
    check((cpu.state.d[0] & 0xFF) == 0x42, "ABCD 15+27=42 BCD");

    // ABCD with carry: 99+01 = 00 with carry
    resetCPU();
    cpu.state.d[0] = 0x99;
    cpu.state.d[1] = 0x01;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xC101);
    check((cpu.state.d[0] & 0xFF) == 0x00, "ABCD 99+01=00 BCD result");
    check(cpu.state.sr & FLAG_C, "ABCD 99+01 carry");

    // SBCD D1,D0: 1000 000 1 00000 001 = 0x8101
    resetCPU();
    cpu.state.d[0] = 0x42;
    cpu.state.d[1] = 0x15;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x8101);
    check((cpu.state.d[0] & 0xFF) == 0x27, "SBCD 42-15=27 BCD");

    endGroup();
}

static void test_addx_subx() {
    beginGroup("ADDX/SUBX");

    // ADDX.L D1,D0: 1101 000 1 10 000 001 = 0xD181
    resetCPU();
    cpu.state.d[0] = 0xFFFFFFFF;
    cpu.state.d[1] = 0x00000001;
    cpu.state.sr |= FLAG_X;
    placeAndExec(0xD181);
    check(cpu.state.d[0] == 0x00000001, "ADDX.L with X result");
    check(cpu.state.sr & FLAG_C, "ADDX.L carry");

    // SUBX.L D1,D0: 1001 000 1 10 000 001 = 0x9181
    resetCPU();
    cpu.state.d[0] = 0x00000002;
    cpu.state.d[1] = 0x00000001;
    cpu.state.sr |= FLAG_X;
    placeAndExec(0x9181);
    check(cpu.state.d[0] == 0x00000000, "SUBX.L with X result");
    // SUBX only clears Z if result non-zero; never sets Z.
    // Z was clear after reset, result is 0, so Z stays clear (preserved).
    check(!(cpu.state.sr & FLAG_Z), "SUBX.L zero result preserves Z (was clear)");

    resetCPU();
    cpu.state.d[0] = 0x00000003;
    cpu.state.d[1] = 0x00000001;
    cpu.state.sr &= ~FLAG_X;
    cpu.state.sr |= FLAG_Z; // Z starts set
    placeAndExec(0x9181);
    check(cpu.state.d[0] == 0x00000002, "SUBX.L no X result");
    check(!(cpu.state.sr & FLAG_Z), "SUBX.L non-zero clears Z");

    endGroup();
}

static void test_scc() {
    beginGroup("Scc");

    // ST D0 (always true): 0101 0000 11 000 000 = 0x50C0
    resetCPU();
    cpu.state.d[0] = 0;
    placeAndExec(0x50C0);
    check((cpu.state.d[0] & 0xFF) == 0xFF, "ST D0 sets byte to FF");

    // SF D0 (always false): 0101 0001 11 000 000 = 0x51C0
    resetCPU();
    cpu.state.d[0] = 0xFF;
    placeAndExec(0x51C0);
    check((cpu.state.d[0] & 0xFF) == 0x00, "SF D0 clears byte to 00");

    // SEQ D0 with Z set: 0101 0111 11 000 000 = 0x57C0
    resetCPU();
    cpu.state.d[0] = 0;
    cpu.state.sr |= FLAG_Z;
    placeAndExec(0x57C0);
    check((cpu.state.d[0] & 0xFF) == 0xFF, "SEQ D0 Z=1 sets byte to FF");

    endGroup();
}

static void test_link_unlk() {
    beginGroup("LINK/UNLK");

    // LINK A6,#-8: 0100 1110 01010 110 = 0x4E56, ext = 0xFFF8
    resetCPU();
    cpu.state.a[6] = 0x00100000;
    u32 startSP = cpu.state.a[7];
    placeAndExec2(0x4E56, 0xFFF8);
    check(cpu.state.a[7] == startSP - 4 - 8, "LINK A6,#-8 SP adjusted");
    check(cpu.state.a[6] == startSP - 4, "LINK A6 = old SP after push");

    // UNLK A6: 0100 1110 01011 110 = 0x4E5E
    u32 fp = cpu.state.a[6];
    placeAndExec(0x4E5E);
    check(cpu.state.a[7] == startSP, "UNLK restored SP");
    check(cpu.state.a[6] == 0x00100000, "UNLK restored A6");

    endGroup();
}

static void test_movem() {
    beginGroup("MOVEM");

    // MOVEM.L D0-D3,-(A7): 0100 1000 11 100 111 = 0x48E7
    // register mask follows: D0-D3 = bits 15-12 for predecrement
    resetCPU();
    cpu.state.d[0] = 0x11111111;
    cpu.state.d[1] = 0x22222222;
    cpu.state.d[2] = 0x33333333;
    cpu.state.d[3] = 0x44444444;
    u32 startSP = cpu.state.a[7];
    placeAndExec2(0x48E7, 0xF000); // D0-D3 in predecrement order
    check(cpu.state.a[7] == startSP - 16, "MOVEM.L regs to stack");

    // MOVEM.L (A7)+,D4-D7: 0100 1100 11 011 111 = 0x4CDF
    // register mask: D4-D7 = bits 4-7 for postincrement (normal order)
    // Actually for (An)+, mask is D0=bit0..D7=bit7,A0=bit8..A7=bit15
    // D4-D7 = bits 4,5,6,7 = 0x00F0
    placeAndExec2(0x4CDF, 0x00F0);
    check(cpu.state.d[4] == 0x11111111, "MOVEM.L from stack D4");
    check(cpu.state.d[5] == 0x22222222, "MOVEM.L from stack D5");
    check(cpu.state.d[6] == 0x33333333, "MOVEM.L from stack D6");
    check(cpu.state.d[7] == 0x44444444, "MOVEM.L from stack D7");
    check(cpu.state.a[7] == startSP, "MOVEM.L restored SP");

    endGroup();
}

static void test_chk() {
    beginGroup("CHK");

    // CHK D1,D0: 0100 000 110 000 001 = 0x4181
    resetCPU();
    cpu.state.d[0] = 5;
    cpu.state.d[1] = 10;
    // Set up CHK exception vector (vector 6)
    writeLong(6 * 4, 0x00005000);
    placeAndExec(0x4181);
    // Value 5 is within bounds [0, 10], no exception
    check(cpu.state.pc != 0x5000, "CHK in bounds - no exception");

    endGroup();
}

static void test_nbcd() {
    beginGroup("NBCD");

    // NBCD D0: 0100 1000 00 000 000 = 0x4800
    resetCPU();
    cpu.state.d[0] = 0x25;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x4800);
    check((cpu.state.d[0] & 0xFF) == 0x75, "NBCD 0-0x25=0x75 BCD");
    check(cpu.state.sr & FLAG_C, "NBCD borrow");

    resetCPU();
    cpu.state.d[0] = 0x00;
    cpu.state.sr |= FLAG_X;
    cpu.state.sr |= FLAG_Z;
    placeAndExec(0x4800);
    check((cpu.state.d[0] & 0xFF) == 0x99, "NBCD 0-0x00-X=0x99 BCD");
    check(cpu.state.sr & FLAG_C, "NBCD with X sets borrow");
    check(!(cpu.state.sr & FLAG_Z), "NBCD non-zero result clears sticky Z");

    resetCPU();
    cpu.state.d[0] = 0x00;
    cpu.state.sr |= FLAG_Z;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x4800);
    check((cpu.state.d[0] & 0xFF) == 0x00, "NBCD 0-0x00=0x00");
    check(!(cpu.state.sr & FLAG_C), "NBCD zero clears borrow");
    check(cpu.state.sr & FLAG_Z, "NBCD zero preserves sticky Z");

    resetCPU();
    cpu.state.a[0] = 0x4001;
    memory[0x4000] = 0x42;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0x4820); // NBCD -(A0)
    check(cpu.state.a[0] == 0x4000, "NBCD predecrement updates address register");
    check(memory[0x4000] == 0x58, "NBCD memory mode writes BCD result");
    check(cpu.state.sr & FLAG_C, "NBCD memory mode sets borrow");

    endGroup();
}

static void test_tas() {
    beginGroup("TAS");

    // TAS D0: 0100 1010 11 000 000 = 0x4AC0
    resetCPU();
    cpu.state.d[0] = 0x00;
    placeAndExec(0x4AC0);
    check((cpu.state.d[0] & 0xFF) == 0x80, "TAS set bit 7");
    check(cpu.state.sr & FLAG_Z, "TAS Z flag (was zero before set)");

    resetCPU();
    cpu.state.d[0] = 0x42;
    placeAndExec(0x4AC0);
    check((cpu.state.d[0] & 0xFF) == 0xC2, "TAS set bit 7 on 0x42");
    check(!(cpu.state.sr & FLAG_Z), "TAS Z clear (was non-zero)");

    endGroup();
}

static void test_memory_ops() {
    beginGroup("Memory addressing");

    // MOVE.L #$DEADBEEF,(A0) then read it back
    resetCPU();
    cpu.state.a[0] = 0x4000;
    // MOVE.L #imm,(A0): 0010 000 010 111 100 = 0x20BC
    placeAndExec3(0x20BC, 0xDEAD, 0xBEEF);
    u32 val = (static_cast<u32>(memory[0x4000]) << 24) |
              (static_cast<u32>(memory[0x4001]) << 16) |
              (static_cast<u32>(memory[0x4002]) << 8) |
              memory[0x4003];
    check(val == 0xDEADBEEF, "MOVE.L #imm,(A0) wrote to memory");

    // Read back with MOVE.L (A0),D0: 0010 000 000 010 000 = 0x2010
    placeAndExec(0x2010);
    check(cpu.state.d[0] == 0xDEADBEEF, "MOVE.L (A0),D0 read from memory");

    // Post-increment: MOVE.W (A0)+,D0
    // 0011 000 000 011 000 = 0x3018
    resetCPU();
    cpu.state.a[0] = 0x4000;
    writeWord(0x4000, 0x1234);
    placeAndExec(0x3018);
    check((cpu.state.d[0] & 0xFFFF) == 0x1234, "MOVE.W (A0)+,D0 value");
    check(cpu.state.a[0] == 0x4002, "Post-increment A0");

    // Pre-decrement: MOVE.W -(A0),D0
    // 0011 000 000 100 000 = 0x3020
    resetCPU();
    cpu.state.a[0] = 0x4002;
    writeWord(0x4000, 0x5678);
    placeAndExec(0x3020);
    check((cpu.state.d[0] & 0xFFFF) == 0x5678, "MOVE.W -(A0),D0 value");
    check(cpu.state.a[0] == 0x4000, "Pre-decrement A0");

    endGroup();
}

static void test_shift_long_timing() {
    beginGroup("Shift/Rotate long timing");

    // Verify that all shift/rotate register operations use base 8 for long size
    // We test with count=0 (register count, register contains 0)

    // ASL.L D1,D0 (count from reg): 1110 001 1 10 1 00 000 = 0xE3A0
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    int cycles = placeAndExec(0xE3A0);
    check(cycles == 8, "ASL.L count=0 base timing = 8");

    // LSR.L D1,D0: 1110 001 0 10 1 01 000 = 0xE2A8
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xE2A8);
    check(cycles == 8, "LSR.L count=0 base timing = 8");

    // ROL.L D1,D0: 1110 001 1 10 1 11 000 = 0xE3B8
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xE3B8);
    check(cycles == 8, "ROL.L count=0 base timing = 8");

    // ROXL.L D1,D0: 1110 001 1 10 1 10 000 = 0xE3B0
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xE3B0);
    check(cycles == 8, "ROXL.L count=0 base timing = 8");

    // Verify byte/word still uses base 6
    // ASL.W D1,D0: 1110 001 1 01 1 00 000 = 0xE360
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xE360);
    check(cycles == 6, "ASL.W count=0 base timing = 6");

    // ASL.B D1,D0: 1110 001 1 00 1 00 000 = 0xE320
    resetCPU();
    cpu.state.d[0] = 1;
    cpu.state.d[1] = 0;
    cycles = placeAndExec(0xE320);
    check(cycles == 6, "ASL.B count=0 base timing = 6");

    // Long with count=5: should be 8+10=18
    // ASL.L #5,D0: 1110 101 1 10 1 00 000 = 0xEB80
    resetCPU();
    cpu.state.d[0] = 1;
    cycles = placeAndExec(0xEB80);
    check(cycles == 18, "ASL.L #5 cycles (8+2*5=18)");

    // Word with count=5: should be 6+10=16
    // ASL.W #5,D0: 1110 101 1 01 1 00 000 = 0xEB40
    resetCPU();
    cpu.state.d[0] = 1;
    cycles = placeAndExec(0xEB40);
    check(cycles == 16, "ASL.W #5 cycles (6+2*5=16)");

    endGroup();
}

static void test_divu_by_zero() {
    beginGroup("DIVU/DIVS divide by zero");

    // DIVU D1,D0 with D1=0
    resetCPU();
    cpu.state.d[0] = 100;
    cpu.state.d[1] = 0;
    writeLong(5 * 4, 0x00006000); // div-by-zero vector
    placeAndExec(0x80C1);
    check(cpu.state.pc == 0x6000, "DIVU div-by-zero exception");

    // DIVS D1,D0 with D1=0
    resetCPU();
    cpu.state.d[0] = 100;
    cpu.state.d[1] = 0;
    writeLong(5 * 4, 0x00006000);
    placeAndExec(0x81C1);
    check(cpu.state.pc == 0x6000, "DIVS div-by-zero exception");

    endGroup();
}

static void test_cmpa() {
    beginGroup("CMPA");

    // CMPA.W D0,A0: 1011 000 011 000 000 = 0xB0C0
    resetCPU();
    cpu.state.a[0] = 0x00001000;
    cpu.state.d[0] = 0x1000;
    cpu.state.sr |= FLAG_X;
    placeAndExec(0xB0C0);
    check(cpu.state.sr & FLAG_Z, "CMPA.W equal Z flag");
    check(cpu.state.sr & FLAG_X, "CMPA.W preserves X flag");

    // CMPA.L D0,A0: 1011 000 111 000 000 = 0xB1C0
    resetCPU();
    cpu.state.a[0] = 0x00002000;
    cpu.state.d[0] = 0x00001000;
    cpu.state.sr &= ~FLAG_X;
    placeAndExec(0xB1C0);
    check(!(cpu.state.sr & FLAG_Z), "CMPA.L not equal");
    check(!(cpu.state.sr & FLAG_N), "CMPA.L A0>D0 N clear");
    check(!(cpu.state.sr & FLAG_X), "CMPA.L preserves cleared X flag");

    endGroup();
}

static void test_adda_suba() {
    beginGroup("ADDA/SUBA");

    // ADDA.W D0,A0: 1101 000 011 000 000 = 0xD0C0
    resetCPU();
    cpu.state.a[0] = 0x1000;
    cpu.state.d[0] = 0x0100;
    placeAndExec(0xD0C0);
    check(cpu.state.a[0] == 0x1100, "ADDA.W result");

    // SUBA.W D0,A0: 1001 000 011 000 000 = 0x90C0
    resetCPU();
    cpu.state.a[0] = 0x2000;
    cpu.state.d[0] = 0x0100;
    placeAndExec(0x90C0);
    check(cpu.state.a[0] == 0x1F00, "SUBA.W result");

    // ADDA.W sign-extends: D0=0x8000 (-32768)
    resetCPU();
    cpu.state.a[0] = 0x00010000;
    cpu.state.d[0] = 0x8000;
    placeAndExec(0xD0C0);
    check(cpu.state.a[0] == 0x00010000 + 0xFFFF8000, "ADDA.W sign extends");

    endGroup();
}

// Comprehensive instruction timing tests based on yacht.txt v1.1
static void test_instruction_timing() {
    beginGroup("Instruction timing (yacht.txt v1.1)");
    int cycles;

    // --- MOVE timing ---
    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec(0x3200); // MOVE.W D0,D1
    check(cycles == 4, "MOVE.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec(0x2200); // MOVE.L D0,D1
    check(cycles == 4, "MOVE.L Dn,Dn = 4");

    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0x3010); // MOVE.W (A0),D0
    check(cycles == 8, "MOVE.W (An),Dn = 8");

    resetCPU(); cpu.state.a[0] = 0x4000; writeLong(0x4000, 0x12345678);
    cycles = placeAndExec(0x2010); // MOVE.L (A0),D0
    check(cycles == 12, "MOVE.L (An),Dn = 12");

    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0x3018); // MOVE.W (A0)+,D0
    check(cycles == 8, "MOVE.W (An)+,Dn = 8");

    resetCPU(); cpu.state.a[0] = 0x4002; writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0x3020); // MOVE.W -(A0),D0
    check(cycles == 10, "MOVE.W -(An),Dn = 10");

    resetCPU(); cpu.state.a[0] = 0x4004; writeLong(0x4000, 0x12345678);
    cycles = placeAndExec(0x2020); // MOVE.L -(A0),D0
    check(cycles == 14, "MOVE.L -(An),Dn = 14");

    resetCPU();
    cycles = placeAndExec2(0x303C, 0x1234); // MOVE.W #$1234,D0
    check(cycles == 8, "MOVE.W #imm,Dn = 8");

    resetCPU();
    cycles = placeAndExec3(0x203C, 0x1234, 0x5678); // MOVE.L #$12345678,D0
    check(cycles == 12, "MOVE.L #imm,Dn = 12");

    resetCPU(); cpu.state.d[0] = 0x1234; cpu.state.a[1] = 0x4000;
    cycles = placeAndExec(0x3280); // MOVE.W D0,(A1)
    check(cycles == 8, "MOVE.W Dn,(An) = 8");

    resetCPU(); cpu.state.d[0] = 0x12345678; cpu.state.a[1] = 0x4000;
    cycles = placeAndExec(0x2280); // MOVE.L D0,(A1)
    check(cycles == 12, "MOVE.L Dn,(An) = 12");

    resetCPU(); cpu.state.d[0] = 0x1234; cpu.state.a[1] = 0x4002;
    cycles = placeAndExec(0x3300); // MOVE.W D0,-(A1)
    check(cycles == 8, "MOVE.W Dn,-(An) = 8");

    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4004, 0x5678);
    cycles = placeAndExec2(0x3028, 0x0004); // MOVE.W 4(A0),D0
    check(cycles == 12, "MOVE.W (d16,An),Dn = 12");

    resetCPU(); cpu.state.a[0] = 0x4000; writeLong(0x4004, 0x12345678);
    cycles = placeAndExec2(0x2028, 0x0004); // MOVE.L 4(A0),D0
    check(cycles == 16, "MOVE.L (d16,An),Dn = 16");

    resetCPU(); cpu.state.d[0] = 0x1234; cpu.state.a[1] = 0x4000;
    cycles = placeAndExec2(0x3340, 0x0004); // MOVE.W D0,4(A1)
    check(cycles == 12, "MOVE.W Dn,(d16,An) = 12");

    resetCPU(); cpu.state.d[0] = 0x12345678; cpu.state.a[1] = 0x4000;
    cycles = placeAndExec2(0x2340, 0x0004); // MOVE.L D0,4(A1)
    check(cycles == 16, "MOVE.L Dn,(d16,An) = 16");

    resetCPU(); writeWord(0x4000, 0x1234);
    cycles = placeAndExec2(0x3038, 0x4000); // MOVE.W $4000.W,D0
    check(cycles == 12, "MOVE.W (xxx).W,Dn = 12");

    resetCPU(); writeWord(0x4000, 0x1234);
    cycles = placeAndExec3(0x3039, 0x0000, 0x4000); // MOVE.W $4000.L,D0
    check(cycles == 16, "MOVE.W (xxx).L,Dn = 16");

    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec2(0x31C0, 0x4000); // MOVE.W D0,$4000.W
    check(cycles == 12, "MOVE.W Dn,(xxx).W = 12");

    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec3(0x33C0, 0x0000, 0x4000); // MOVE.W D0,$4000.L
    check(cycles == 16, "MOVE.W Dn,(xxx).L = 16");

    // MOVEA.W/L Dn,An: 4
    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec(0x3040); // MOVEA.W D0,A0
    check(cycles == 4, "MOVEA.W Dn,An = 4");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec(0x2040); // MOVEA.L D0,A0
    check(cycles == 4, "MOVEA.L Dn,An = 4");

    // --- ADD/SUB timing ---
    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2;
    cycles = placeAndExec(0xD041); // ADD.W D1,D0
    check(cycles == 4, "ADD.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2;
    cycles = placeAndExec(0xD081); // ADD.L D1,D0
    check(cycles == 8, "ADD.L Dn,Dn = 8");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.a[1] = 0x4000; writeWord(0x4000, 2);
    cycles = placeAndExec(0xD051); // ADD.W (A1),D0
    check(cycles == 8, "ADD.W (An),Dn = 8");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.a[1] = 0x4000; writeLong(0x4000, 2);
    cycles = placeAndExec(0xD091); // ADD.L (A1),D0
    // yacht.txt: .L memory source base=6, +8 EA = 14
    check(cycles == 14, "ADD.L (An),Dn = 14");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec2(0xD07C, 0x0002); // ADD.W #2,D0
    check(cycles == 8, "ADD.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec3(0xD0BC, 0x0000, 0x0002); // ADD.L #2,D0
    check(cycles == 16, "ADD.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.a[1] = 0x4000; writeWord(0x4000, 2);
    cycles = placeAndExec(0xD151); // ADD.W D0,(A1)
    check(cycles == 12, "ADD.W Dn,(An) = 12");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.a[1] = 0x4000; writeLong(0x4000, 2);
    cycles = placeAndExec(0xD191); // ADD.L D0,(A1)
    check(cycles == 20, "ADD.L Dn,(An) = 20");

    resetCPU(); cpu.state.d[0] = 5; cpu.state.d[1] = 2;
    cycles = placeAndExec(0x9041); // SUB.W D1,D0
    check(cycles == 4, "SUB.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 5; cpu.state.d[1] = 2;
    cycles = placeAndExec(0x9081); // SUB.L D1,D0
    check(cycles == 8, "SUB.L Dn,Dn = 8");

    // --- ADDI/SUBI/ORI/ANDI/EORI/CMPI timing ---
    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec2(0x0640, 0x0002); // ADDI.W #2,D0
    check(cycles == 8, "ADDI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec3(0x0680, 0x0000, 0x0002); // ADDI.L #2,D0
    check(cycles == 16, "ADDI.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 5;
    cycles = placeAndExec2(0x0440, 0x0002); // SUBI.W #2,D0
    check(cycles == 8, "SUBI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 5;
    cycles = placeAndExec3(0x0480, 0x0000, 0x0002); // SUBI.L #2,D0
    check(cycles == 16, "SUBI.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec2(0x0040, 0x00FF); // ORI.W #$FF,D0
    check(cycles == 8, "ORI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec3(0x0080, 0x0000, 0x00FF); // ORI.L #$FF,D0
    check(cycles == 16, "ORI.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 0xFFFF;
    cycles = placeAndExec2(0x0240, 0x00FF); // ANDI.W #$FF,D0
    check(cycles == 8, "ANDI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0xFFFFFFFF;
    cycles = placeAndExec3(0x0280, 0x0000, 0x00FF); // ANDI.L #$FF,D0
    check(cycles == 16, "ANDI.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec2(0x0A40, 0xFFFF); // EORI.W #$FFFF,D0
    check(cycles == 8, "EORI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec3(0x0A80, 0xFFFF, 0xFFFF); // EORI.L #$FFFFFFFF,D0
    check(cycles == 16, "EORI.L #imm,Dn = 16");

    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec2(0x0C40, 0x1234); // CMPI.W #$1234,D0
    check(cycles == 8, "CMPI.W #imm,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec3(0x0C80, 0x1234, 0x5678); // CMPI.L #$12345678,D0
    check(cycles == 14, "CMPI.L #imm,Dn = 14");

    // ADDI/SUBI with memory EA
    // yacht.txt: ADDI.W #,(An)=12+4=16, ADDI.L #,(An)=20+8=28
    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4000, 1);
    cycles = placeAndExec2(0x0650, 0x0002); // ADDI.W #2,(A0)
    check(cycles == 16, "ADDI.W #imm,(An) = 16");

    resetCPU(); cpu.state.a[0] = 0x4000; writeLong(0x4000, 1);
    cycles = placeAndExec3(0x0690, 0x0000, 0x0002); // ADDI.L #2,(A0)
    check(cycles == 28, "ADDI.L #imm,(An) = 28");

    // --- ADDQ/SUBQ timing ---
    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec(0x5640); // ADDQ.W #3,D0
    check(cycles == 4, "ADDQ.W #n,Dn = 4");

    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec(0x5680); // ADDQ.L #3,D0
    check(cycles == 8, "ADDQ.L #n,Dn = 8");

    resetCPU(); cpu.state.a[0] = 0x1000;
    cycles = placeAndExec(0x5648); // ADDQ.W #3,A0
    check(cycles == 8, "ADDQ.W #n,An = 8");

    resetCPU(); writeLong(0x4000, 1);
    cycles = placeAndExec2(0x52B8, 0x4000); // ADDQ.L #1,$4000.W
    check(cycles == 24, "ADDQ.L #n,(xxx).W = 12+EA");
    check(((u32)memory[0x4000] << 24 | (u32)memory[0x4001] << 16 |
           (u32)memory[0x4002] << 8 | (u32)memory[0x4003]) == 2,
          "ADDQ.L #n,(xxx).W result");

    resetCPU(); writeLong(0x4000, 1);
    cycles = placeAndExec3(0x52B9, 0x0000, 0x4000); // ADDQ.L #1,$00004000.L
    check(cycles == 28, "ADDQ.L #n,(xxx).L = 12+EA");
    check(((u32)memory[0x4000] << 24 | (u32)memory[0x4001] << 16 |
           (u32)memory[0x4002] << 8 | (u32)memory[0x4003]) == 2,
          "ADDQ.L #n,(xxx).L result");

    resetCPU(); cpu.state.d[0] = 10;
    cycles = placeAndExec(0x5740); // SUBQ.W #3,D0
    check(cycles == 4, "SUBQ.W #n,Dn = 4");

    resetCPU(); cpu.state.d[0] = 10;
    cycles = placeAndExec(0x5780); // SUBQ.L #3,D0
    check(cycles == 8, "SUBQ.L #n,Dn = 8");

    resetCPU(); writeLong(0x4000, 10);
    cycles = placeAndExec2(0x53B8, 0x4000); // SUBQ.L #1,$4000.W
    check(cycles == 24, "SUBQ.L #n,(xxx).W = 12+EA");
    check(((u32)memory[0x4000] << 24 | (u32)memory[0x4001] << 16 |
           (u32)memory[0x4002] << 8 | (u32)memory[0x4003]) == 9,
          "SUBQ.L #n,(xxx).W result");

    resetCPU(); writeLong(0x4000, 10);
    cycles = placeAndExec3(0x53B9, 0x0000, 0x4000); // SUBQ.L #1,$00004000.L
    check(cycles == 28, "SUBQ.L #n,(xxx).L = 12+EA");
    check(((u32)memory[0x4000] << 24 | (u32)memory[0x4001] << 16 |
           (u32)memory[0x4002] << 8 | (u32)memory[0x4003]) == 9,
          "SUBQ.L #n,(xxx).L result");

    // --- ADDA/SUBA timing ---
    resetCPU(); cpu.state.a[0] = 0x1000; cpu.state.d[0] = 0x100;
    cycles = placeAndExec(0xD0C0); // ADDA.W D0,A0
    check(cycles == 8, "ADDA.W Dn,An = 8");

    resetCPU(); cpu.state.a[0] = 0x1000; cpu.state.d[0] = 0x100;
    cycles = placeAndExec(0xD1C0); // ADDA.L D0,A0
    check(cycles == 8, "ADDA.L Dn,An = 8");

    resetCPU(); cpu.state.a[0] = 0x2000; cpu.state.d[0] = 0x100;
    cycles = placeAndExec(0x90C0); // SUBA.W D0,A0
    check(cycles == 8, "SUBA.W Dn,An = 8");

    resetCPU(); cpu.state.a[0] = 0x2000; cpu.state.d[0] = 0x100;
    cycles = placeAndExec(0x91C0); // SUBA.L D0,A0
    check(cycles == 8, "SUBA.L Dn,An = 8");

    // --- CMP/CMPA/CMPM timing ---
    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2;
    cycles = placeAndExec(0xB041); // CMP.W D1,D0
    check(cycles == 4, "CMP.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2;
    cycles = placeAndExec(0xB081); // CMP.L D1,D0
    check(cycles == 6, "CMP.L Dn,Dn = 6");

    resetCPU(); cpu.state.a[0] = 0x1000; cpu.state.d[0] = 0x1000;
    cycles = placeAndExec(0xB0C0); // CMPA.W D0,A0
    check(cycles == 6, "CMPA.W Dn,An = 6");

    resetCPU(); cpu.state.a[0] = 0x1000; cpu.state.d[0] = 0x1000;
    cycles = placeAndExec(0xB1C0); // CMPA.L D0,A0
    check(cycles == 6, "CMPA.L Dn,An = 6");

    resetCPU();
    cpu.state.a[0] = 0x4000; cpu.state.a[1] = 0x4010;
    writeWord(0x4010, 0x1234); writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0xB149); // CMPM.W (A1)+,(A0)+
    check(cycles == 12, "CMPM.W = 12");

    resetCPU();
    cpu.state.a[0] = 0x4000; cpu.state.a[1] = 0x4010;
    writeLong(0x4010, 0x12345678); writeLong(0x4000, 0x12345678);
    cycles = placeAndExec(0xB189); // CMPM.L (A1)+,(A0)+
    check(cycles == 20, "CMPM.L = 20");

    // --- AND/OR/EOR timing ---
    resetCPU(); cpu.state.d[0] = 0xFF; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0xC041); // AND.W D1,D0
    check(cycles == 4, "AND.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 0xFF; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0xC081); // AND.L D1,D0
    check(cycles == 8, "AND.L Dn,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0xF0; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0x8041); // OR.W D1,D0
    check(cycles == 4, "OR.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 0xF0; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0x8081); // OR.L D1,D0
    check(cycles == 8, "OR.L Dn,Dn = 8");

    resetCPU(); cpu.state.d[0] = 0xFF; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0xB141); // EOR.W D0,D1
    check(cycles == 4, "EOR.W Dn,Dn = 4");

    resetCPU(); cpu.state.d[0] = 0xFF; cpu.state.d[1] = 0x0F;
    cycles = placeAndExec(0xB181); // EOR.L D0,D1
    check(cycles == 8, "EOR.L Dn,Dn = 8");

    // --- CLR/NEG/NOT/NEGX timing ---
    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec(0x4240); // CLR.W D0
    check(cycles == 4, "CLR.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec(0x4280); // CLR.L D0
    check(cycles == 6, "CLR.L Dn = 6");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec(0x4440); // NEG.W D0
    check(cycles == 4, "NEG.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec(0x4480); // NEG.L D0
    check(cycles == 6, "NEG.L Dn = 6");

    resetCPU(); cpu.state.d[0] = 0x1234;
    cycles = placeAndExec(0x4640); // NOT.W D0
    check(cycles == 4, "NOT.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec(0x4680); // NOT.L D0
    check(cycles == 6, "NOT.L Dn = 6");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec(0x4040); // NEGX.W D0
    check(cycles == 4, "NEGX.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 1;
    cycles = placeAndExec(0x4080); // NEGX.L D0
    check(cycles == 6, "NEGX.L Dn = 6");

    // CLR/NEG with memory EA
    // yacht.txt: CLR.W (An)=8+4=12, CLR.L (An)=12+8=20 (read-then-write)
    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0x4250); // CLR.W (A0)
    check(cycles == 12, "CLR.W (An) = 12");

    resetCPU(); cpu.state.a[0] = 0x4000; writeLong(0x4000, 0x12345678);
    cycles = placeAndExec(0x4290); // CLR.L (A0)
    check(cycles == 20, "CLR.L (An) = 20");

    // --- TST timing ---
    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec(0x4A40); // TST.W D0
    check(cycles == 4, "TST.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec(0x4A80); // TST.L D0
    check(cycles == 4, "TST.L Dn = 4");

    resetCPU(); cpu.state.a[0] = 0x4000; writeWord(0x4000, 0x1234);
    cycles = placeAndExec(0x4A50); // TST.W (A0)
    check(cycles == 8, "TST.W (An) = 8");

    resetCPU(); cpu.state.a[0] = 0x4000; writeLong(0x4000, 0x12345678);
    cycles = placeAndExec(0x4A90); // TST.L (A0)
    check(cycles == 12, "TST.L (An) = 12");

    // --- EXT/SWAP/EXG timing ---
    resetCPU(); cpu.state.d[0] = 0x80;
    cycles = placeAndExec(0x4880); // EXT.W D0
    check(cycles == 4, "EXT.W = 4");

    resetCPU(); cpu.state.d[0] = 0xFFFF;
    cycles = placeAndExec(0x48C0); // EXT.L D0
    check(cycles == 4, "EXT.L = 4");

    resetCPU(); cpu.state.d[0] = 0x12345678;
    cycles = placeAndExec(0x4840); // SWAP D0
    check(cycles == 4, "SWAP = 4");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2;
    cycles = placeAndExec(0xC141); // EXG D0,D1
    check(cycles == 6, "EXG Dn,Dn = 6");

    resetCPU(); cpu.state.a[0] = 1; cpu.state.a[1] = 2;
    cycles = placeAndExec(0xC149); // EXG A0,A1
    check(cycles == 6, "EXG An,An = 6");

    // --- ABCD/SBCD/ADDX/SUBX/NBCD timing ---
    resetCPU(); cpu.state.d[0] = 0x15; cpu.state.d[1] = 0x27; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0xC101); // ABCD D1,D0
    check(cycles == 6, "ABCD Dn = 6");

    resetCPU(); cpu.state.d[0] = 0x42; cpu.state.d[1] = 0x15; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x8101); // SBCD D1,D0
    check(cycles == 6, "SBCD Dn = 6");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0xD141); // ADDX.W D1,D0
    check(cycles == 4, "ADDX.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 1; cpu.state.d[1] = 2; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0xD181); // ADDX.L D1,D0
    check(cycles == 8, "ADDX.L Dn = 8");

    resetCPU(); cpu.state.d[0] = 5; cpu.state.d[1] = 2; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x9141); // SUBX.W D1,D0
    check(cycles == 4, "SUBX.W Dn = 4");

    resetCPU(); cpu.state.d[0] = 5; cpu.state.d[1] = 2; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x9181); // SUBX.L D1,D0
    check(cycles == 8, "SUBX.L Dn = 8");

    resetCPU(); cpu.state.d[0] = 0x25; cpu.state.sr &= ~FLAG_X;
    cycles = placeAndExec(0x4800); // NBCD D0
    check(cycles == 6, "NBCD Dn = 6");

    // --- TAS timing ---
    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec(0x4AC0); // TAS D0
    check(cycles == 4, "TAS Dn = 4");

    // --- BTST/BCHG/BCLR/BSET timing ---
    resetCPU(); cpu.state.d[0] = 0; cpu.state.d[1] = 0x01;
    cycles = placeAndExec(0x0101); // BTST D0,D1
    check(cycles == 6, "BTST Dn,Dm = 6");

    resetCPU(); cpu.state.d[0] = 0x01;
    cycles = placeAndExec2(0x0800, 0x0000); // BTST #0,D0
    check(cycles == 10, "BTST #imm,Dn = 10");

    // yacht.txt: BCHG/BSET Dn,Dm bit<16=6, bit>=16=8
    //            BCLR Dn,Dm bit<16=8, bit>=16=10
    resetCPU(); cpu.state.d[0] = 3; cpu.state.d[1] = 0;
    cycles = placeAndExec(0x0141); // BCHG D0,D1 (bit 3 < 16)
    check(cycles == 6, "BCHG Dn,Dm bit<16 = 6");

    resetCPU(); cpu.state.d[0] = 3; cpu.state.d[1] = 0;
    cycles = placeAndExec(0x01C1); // BSET D0,D1 (bit 3 < 16)
    check(cycles == 6, "BSET Dn,Dm bit<16 = 6");

    resetCPU(); cpu.state.d[0] = 3; cpu.state.d[1] = 0xFF;
    cycles = placeAndExec(0x0181); // BCLR D0,D1 (bit 3 < 16)
    check(cycles == 8, "BCLR Dn,Dm bit<16 = 8");

    // yacht.txt: static BCHG/BSET #,Dn bit<16=10, bit>=16=12
    //            static BCLR #,Dn bit<16=12, bit>=16=14
    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec2(0x0840, 0x0003); // BCHG #3,D0 (bit 3 < 16)
    check(cycles == 10, "BCHG #imm,Dn bit<16 = 10");

    resetCPU(); cpu.state.d[0] = 0;
    cycles = placeAndExec2(0x08C0, 0x0003); // BSET #3,D0 (bit 3 < 16)
    check(cycles == 10, "BSET #imm,Dn bit<16 = 10");

    resetCPU(); cpu.state.d[0] = 0xFF;
    cycles = placeAndExec2(0x0880, 0x0003); // BCLR #3,D0 (bit 3 < 16)
    check(cycles == 12, "BCLR #imm,Dn bit<16 = 12");

    // --- LEA timing ---
    resetCPU(); cpu.state.a[0] = 0x5000;
    cycles = placeAndExec(0x43D0); // LEA (A0),A1
    check(cycles == 4, "LEA (An) = 4");

    resetCPU(); cpu.state.a[0] = 0x5000;
    cycles = placeAndExec2(0x43E8, 0x0004); // LEA 4(A0),A1
    check(cycles == 8, "LEA (d16,An) = 8");

    resetCPU(); cpu.state.a[0] = 0x5000; cpu.state.d[0] = 0;
    cycles = placeAndExec2(0x43F0, 0x0000); // LEA (0,A0,D0.W),A1
    check(cycles == 12, "LEA (d8,An,Xn) = 12");

    resetCPU();
    cycles = placeAndExec2(0x43F8, 0x5000); // LEA $5000.W,A1
    check(cycles == 8, "LEA (xxx).W = 8");

    resetCPU();
    cycles = placeAndExec3(0x43F9, 0x0000, 0x5000); // LEA $5000.L,A1
    check(cycles == 12, "LEA (xxx).L = 12");

    // --- PEA timing ---
    resetCPU(); cpu.state.a[0] = 0x5000;
    cycles = placeAndExec(0x4850); // PEA (A0)
    check(cycles == 12, "PEA (An) = 12");

    resetCPU(); cpu.state.a[0] = 0x5000;
    cycles = placeAndExec2(0x4868, 0x0004); // PEA 4(A0)
    check(cycles == 16, "PEA (d16,An) = 16");

    // --- JMP timing ---
    resetCPU(); cpu.state.a[0] = 0x3000; writeWord(0x3000, 0x4E71);
    cycles = placeAndExec(0x4ED0); // JMP (A0)
    check(cycles == 8, "JMP (An) = 8");

    resetCPU(); cpu.state.a[0] = 0x3000; writeWord(0x3004, 0x4E71);
    cycles = placeAndExec2(0x4EE8, 0x0004); // JMP 4(A0)
    check(cycles == 10, "JMP (d16,An) = 10");

    resetCPU(); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec2(0x4EF8, 0x3000); // JMP $3000.W
    check(cycles == 10, "JMP (xxx).W = 10");

    resetCPU(); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec3(0x4EF9, 0x0000, 0x3000); // JMP $3000.L
    check(cycles == 12, "JMP (xxx).L = 12");

    // --- JSR timing ---
    resetCPU(); cpu.state.a[0] = 0x3000; writeWord(0x3000, 0x4E71);
    cycles = placeAndExec(0x4E90); // JSR (A0)
    check(cycles == 16, "JSR (An) = 16");

    resetCPU(); cpu.state.a[0] = 0x3000; writeWord(0x3004, 0x4E71);
    cycles = placeAndExec2(0x4EA8, 0x0004); // JSR 4(A0)
    check(cycles == 18, "JSR (d16,An) = 18");

    resetCPU(); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec2(0x4EB8, 0x3000); // JSR $3000.W
    check(cycles == 18, "JSR (xxx).W = 18");

    resetCPU(); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec3(0x4EB9, 0x0000, 0x3000); // JSR $3000.L
    check(cycles == 24, "JSR (xxx).L = 24");

    // --- BRA/BSR/Bcc timing ---
    resetCPU();
    cycles = placeAndExec(0x6004); // BRA.B +4
    check(cycles == 10, "BRA.B = 10");

    resetCPU();
    cycles = placeAndExec2(0x6000, 0x0004); // BRA.W +4
    check(cycles == 10, "BRA.W = 10");

    resetCPU();
    cycles = placeAndExec(0x6106); // BSR.B +6
    check(cycles == 18, "BSR.B = 18");

    resetCPU();
    cycles = placeAndExec2(0x6100, 0x0006); // BSR.W +6
    check(cycles == 18, "BSR.W = 18");

    resetCPU(); cpu.state.sr |= FLAG_Z;
    cycles = placeAndExec(0x6704); // BEQ.B taken
    check(cycles == 10, "Bcc.B taken = 10");

    resetCPU(); cpu.state.sr &= ~FLAG_Z;
    cycles = placeAndExec(0x6704); // BEQ.B not taken
    check(cycles == 8, "Bcc.B not taken = 8");

    resetCPU(); cpu.state.sr |= FLAG_Z;
    cycles = placeAndExec2(0x6700, 0x0004); // BEQ.W taken
    check(cycles == 10, "Bcc.W taken = 10");

    resetCPU(); cpu.state.sr &= ~FLAG_Z;
    cycles = placeAndExec2(0x6700, 0x0004); // BEQ.W not taken
    check(cycles == 12, "Bcc.W not taken = 12");

    // --- DBcc timing ---
    resetCPU(); cpu.state.d[0] = 3; cpu.state.sr &= ~FLAG_Z;
    cycles = placeAndExec2(0x57C8, 0x0004); // DBEQ D0,+4 (cc false, counter>0)
    check(cycles == 10, "DBcc branch taken = 10");

    resetCPU(); cpu.state.d[0] = 3; cpu.state.sr |= FLAG_Z;
    cycles = placeAndExec2(0x57C8, 0x0004); // DBEQ D0 (cc true)
    check(cycles == 12, "DBcc cc true = 12");

    resetCPU(); cpu.state.d[0] = 0; cpu.state.sr &= ~FLAG_Z;
    cycles = placeAndExec2(0x57C8, 0x0004); // DBEQ D0 (counter expired)
    check(cycles == 14, "DBcc counter expired = 14");

    // --- Scc timing ---
    resetCPU(); cpu.state.d[0] = 0xFF; cpu.state.sr &= ~FLAG_Z;
    cycles = placeAndExec(0x57C0); // SEQ D0 (cc false)
    check(cycles == 4, "Scc false Dn = 4");

    resetCPU(); cpu.state.d[0] = 0; cpu.state.sr |= FLAG_Z;
    cycles = placeAndExec(0x57C0); // SEQ D0 (cc true)
    check(cycles == 6, "Scc true Dn = 6");

    // --- RTS/RTE/LINK/UNLK timing ---
    resetCPU();
    cpu.state.a[7] -= 4;
    writeLong(cpu.state.a[7], 0x00003000); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec(0x4E75); // RTS
    check(cycles == 16, "RTS = 16");

    resetCPU();
    cpu.state.a[7] -= 6;
    writeWord(cpu.state.a[7], 0x2700);
    writeLong(cpu.state.a[7] + 2, 0x00003000); writeWord(0x3000, 0x4E71);
    cycles = placeAndExec(0x4E73); // RTE
    check(cycles == 20, "RTE = 20");

    resetCPU(); cpu.state.a[6] = 0x00100000;
    cycles = placeAndExec2(0x4E56, 0xFFF8); // LINK A6,#-8
    check(cycles == 16, "LINK = 16");

    cycles = placeAndExec(0x4E5E); // UNLK A6
    check(cycles == 12, "UNLK = 12");

    // --- TRAP timing ---
    // yacht.txt: TRAP = 34 total
    resetCPU();
    writeLong(32 * 4, 0x00004000); writeWord(0x4000, 0x4E71);
    cycles = placeAndExec(0x4E40); // TRAP #0
    check(cycles == 34, "TRAP = 34");

    // --- MOVE SR/CCR timing ---
    resetCPU(); cpu.state.sr = 0x2700;
    cycles = placeAndExec(0x40C0); // MOVE SR,D0
    check(cycles == 6, "MOVE SR,Dn = 6");

    resetCPU(); cpu.state.d[0] = 0x001F;
    cycles = placeAndExec(0x44C0); // MOVE D0,CCR
    check(cycles == 12, "MOVE Dn,CCR = 12");

    // --- CHK no trap timing ---
    resetCPU(); cpu.state.d[0] = 5; cpu.state.d[1] = 10;
    cycles = placeAndExec(0x4181); // CHK D1,D0 (in range)
    check(cycles == 10, "CHK no trap = 10");

    // --- MOVEM timing ---
    // MOVEM.W R->M (An), 2 regs: 8+4*2=16
    resetCPU(); cpu.state.d[0] = 0x1111; cpu.state.d[1] = 0x2222; cpu.state.a[0] = 0x4000;
    cycles = placeAndExec2(0x4890, 0x0003); // MOVEM.W D0-D1,(A0)
    check(cycles == 16, "MOVEM.W R->M (An) 2 regs = 16");

    // MOVEM.L R->M -(An), 4 regs: 8+8*4=40
    resetCPU();
    cpu.state.d[0] = 0x11111111; cpu.state.d[1] = 0x22222222;
    cpu.state.d[2] = 0x33333333; cpu.state.d[3] = 0x44444444;
    cycles = placeAndExec2(0x48E7, 0xF000); // MOVEM.L D0-D3,-(A7)
    check(cycles == 40, "MOVEM.L R->M -(An) 4 regs = 40");

    // MOVEM.W M->R (An)+, 2 regs: 12+4*2=20
    resetCPU(); cpu.state.a[0] = 0x4000;
    writeWord(0x4000, 0x1111); writeWord(0x4002, 0x2222); writeWord(0x4004, 0);
    cycles = placeAndExec2(0x4C98, 0x0003); // MOVEM.W (A0)+,D0-D1
    check(cycles == 20, "MOVEM.W M->R (An)+ 2 regs = 20");

    // MOVEM.L M->R (An)+, 2 regs: 12+8*2=28
    resetCPU(); cpu.state.a[0] = 0x4000;
    writeLong(0x4000, 0x11111111); writeLong(0x4004, 0x22222222); writeLong(0x4008, 0);
    cycles = placeAndExec2(0x4CD8, 0x0003); // MOVEM.L (A0)+,D0-D1
    check(cycles == 28, "MOVEM.L M->R (An)+ 2 regs = 28");

    endGroup();
}

// Main
int main() {
#ifdef _MSC_VER
    _putenv_s("GENESIS_M68K_PREFETCH_CREDIT", "0");
#else
    putenv(const_cast<char*>("GENESIS_M68K_PREFETCH_CREDIT=0"));
#endif
    printf("=== M68K CPU Test Suite ===\n\n");

    cpu.connectBus(&bus);

    // Data movement
    test_moveq();
    test_move();
    test_movea();
    test_movem();
    test_movem_bus_timing();
    test_movep_bus_timing();
    test_move_sr();
    test_operand_read_timing();
    test_operand_write_timing();
    test_immediate_memory_alu_timing();
    test_static_bit_memory_timing();
    test_read_modify_write_memory_timing();
    test_memory_shift_rotate_timing();
    test_decimal_memory_timing();
    test_x_memory_predecrement_timing();
    test_data_movement_memory_timing();
    test_predecrement_read_timing();
    test_exg();
    test_swap();
    test_ext();
    test_lea_pea();

    // Arithmetic
    test_add();
    test_sub();
    test_addi_subi_cmpi();
    test_addq_subq();
    test_addx_subx();
    test_adda_suba();
    test_mulu();
    test_muls();
    test_divu();
    test_divs();
    test_divu_by_zero();
    test_cmp();
    test_cmpa();
    test_neg();
    test_clr();
    test_abcd_sbcd();
    test_nbcd();
    test_chk();

    // Logic
    test_and();
    test_or();
    test_eor();
    test_not();
    test_tst();
    test_btst();
    test_ori_andi_eori();

    // Shifts/Rotates
    test_asl_asr();
    test_lsl_lsr();
    test_rol_ror();
    test_roxl_roxr();
    test_shift_long_timing();

    // Branch/Control
    test_bra_bcc();
    test_dbcc();
    test_bsr_rts();
    test_jmp_jsr();
    test_link_unlk();
    test_trap();
    test_nop();
    test_scc();
    test_scc_memory_timing();
    test_tas();
    test_tas_memory_timing();

    // Memory addressing
    test_memory_ops();

    // Comprehensive timing (yacht.txt v1.1)
    test_instruction_timing();

    printf("\n=== Summary ===\n");
    printf("Total: %d  Passed: %d  Failed: %d\n", totalTests, passedTests, failedTests);
    printf("Result: %s\n", failedTests == 0 ? "ALL PASSED" : "FAILURES DETECTED");

    return failedTests == 0 ? 0 : 1;
}
