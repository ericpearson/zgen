// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// M68K CPU instruction test suite
// Self-contained exerciser that stubs the Bus and tests instructions directly.

#include "cpu/m68k.h"
#include "memory/bus.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// Flat 16MB memory for standalone M68K testing
static u8 memory[0x1000000]; // 16MB

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
u16 Bus::read16(u32 addr) {
    addr &= 0xFFFFFE;
    return (static_cast<u16>(memory[addr]) << 8) | memory[addr + 1];
}
void Bus::write8(u32 addr, u8 val) { memory[addr & 0xFFFFFF] = val; }
void Bus::write16(u32 addr, u16 val) {
    addr &= 0xFFFFFE;
    memory[addr] = val >> 8;
    memory[addr + 1] = val & 0xFF;
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

    // Cycle timing: all-zero quotient = minimum
    resetCPU();
    cpu.state.d[0] = 0; // dividend=0
    cpu.state.d[1] = 1; // divisor=1
    cycles = placeAndExec(0x80C1);
    check((cpu.state.d[0] & 0xFFFF) == 0, "DIVU 0/1 quotient");
    // Updated model targets the documented 76-140 DIVU range.
    // quotient = 0 (popcount 0) => 76 + 4*0 = 76
    check(cycles == 76, "DIVU min cycles (76)");

    // All quotient bits 1: max cycle
    resetCPU();
    cpu.state.d[0] = 0xFFFF; // dividend=65535
    cpu.state.d[1] = 1;      // divisor=1
    cycles = placeAndExec(0x80C1);
    // quotient = 0xFFFF (popcount 16) => 76 + 4*16 = 140
    check(cycles == 140, "DIVU max cycles (140)");

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
    placeAndExec(0x81C1);
    check(static_cast<s16>(cpu.state.d[0] & 0xFFFF) == -100, "DIVS -1000/10 quotient");
    check(static_cast<s16>(cpu.state.d[0] >> 16) == 0, "DIVS -1000/10 remainder");

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
    placeAndExec(0x81C1);
    check(static_cast<s16>(cpu.state.d[0] & 0xFFFF) == -10, "DIVS 100/-10 quotient");
    check(static_cast<s16>(cpu.state.d[0] >> 16) == 0, "DIVS 100/-10 remainder");

    endGroup();
}

static void test_cmp() {
    beginGroup("CMP");

    resetCPU();
    cpu.state.d[0] = 0x42;
    cpu.state.d[1] = 0x42;
    // CMP.L D1,D0: 1011 000 010 000 001 = 0xB081
    placeAndExec(0xB081);
    check(cpu.state.sr & FLAG_Z, "CMP equal Z flag");
    check(!(cpu.state.sr & FLAG_N), "CMP equal N flag clear");
    check(!(cpu.state.sr & FLAG_C), "CMP equal C flag clear");

    resetCPU();
    cpu.state.d[0] = 0x10;
    cpu.state.d[1] = 0x20;
    placeAndExec(0xB081);
    check(!(cpu.state.sr & FLAG_Z), "CMP less Z clear");
    check(cpu.state.sr & FLAG_C, "CMP less C flag (borrow)");

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
    placeAndExec2(0x0C40, 0x0042);
    check(cpu.state.sr & FLAG_Z, "CMPI.W equal Z flag");

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
    placeAndExec(0xB0C0);
    check(cpu.state.sr & FLAG_Z, "CMPA.W equal Z flag");

    // CMPA.L D0,A0: 1011 000 111 000 000 = 0xB1C0
    resetCPU();
    cpu.state.a[0] = 0x00002000;
    cpu.state.d[0] = 0x00001000;
    placeAndExec(0xB1C0);
    check(!(cpu.state.sr & FLAG_Z), "CMPA.L not equal");
    check(!(cpu.state.sr & FLAG_N), "CMPA.L A0>D0 N clear");

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

// Main
int main() {
#ifdef _MSC_VER
    _putenv_s("GENESIS_M68K_PREFETCH_CREDIT", "0");
#else
    setenv("GENESIS_M68K_PREFETCH_CREDIT", "0", 1);
#endif
    printf("=== M68K CPU Test Suite ===\n\n");

    cpu.connectBus(&bus);

    // Data movement
    test_moveq();
    test_move();
    test_movea();
    test_movem();
    test_move_sr();
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
    test_tas();

    // Memory addressing
    test_memory_ops();

    printf("\n=== Summary ===\n");
    printf("Total: %d  Passed: %d  Failed: %d\n", totalTests, passedTests, failedTests);
    printf("Result: %s\n", failedTests == 0 ? "ALL PASSED" : "FAILURES DETECTED");

    return failedTests == 0 ? 0 : 1;
}
