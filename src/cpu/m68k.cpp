// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"
#include "memory/bus.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

M68K::M68K() : bus(nullptr),
               prefetchValid_(false),
               prefetchWord0_(0),
               prefetchWord1_(0),
               prefetchPC_(0),
               prefetchNextPC_(0),
               lastPC_(0),
               lastIR_(0) {
    std::memset(&state, 0, sizeof(state));
    state.pendingInterrupt = -1;
}

void M68K::connectBus(Bus* b) {
    bus = b;
}

void M68K::reset() {
    std::memset(&state, 0, sizeof(state));
    state.pendingInterrupt = -1;
    invalidatePrefetch();
    lastPC_ = 0;
    lastIR_ = 0;
    totalCycles_ = 0;

    // Read initial SSP and PC from vector table
    state.ssp = read32(0x000000);
    state.pc = read32(0x000004);
    state.a[7] = state.ssp;

    // Start in supervisor mode with interrupts masked
    state.sr = FLAG_S | (7 << 8);
    state.stopped = false;
    state.halted = false;
}

u8 M68K::read8(u32 addr) {
    return bus->read8(addr & 0xFFFFFF);
}

u16 M68K::read16(u32 addr) {
    if (addr & 1) {
        // Address error: vector 3, 50 cycles per PRM (exception() adds 34)
        exception(3);
        state.cycles += 16;
    }
    return bus->read16(addr & 0xFFFFFE);
}

u8 M68K::read8Timed(u32 addr, int partialCycles) {
    return bus->read8(addr & 0xFFFFFF, partialCycles);
}

u16 M68K::read16Timed(u32 addr, int partialCycles) {
    if (addr & 1) {
        exception(3);
        state.cycles += 16;
    }
    return bus->read16(addr & 0xFFFFFE, partialCycles);
}

u32 M68K::read32Timed(u32 addr, int partialCycles) {
    u16 high = read16Timed(addr, partialCycles);
    u16 low = read16Timed(addr + 2, partialCycles + 4);
    return makeLong(high, low);
}

u32 M68K::read32(u32 addr) {
    u16 high = read16(addr);
    u16 low = read16(addr + 2);
    return makeLong(high, low);
}

void M68K::write8(u32 addr, u8 val) {
    bus->write8(addr & 0xFFFFFF, val);
}

void M68K::write16(u32 addr, u16 val) {
    if (addr & 1) {
        // Address error: 50 cycles per PRM (exception() adds 34)
        exception(3);
        state.cycles += 16;
    }
    bus->write16(addr & 0xFFFFFE, val);
}

void M68K::write32(u32 addr, u32 val) {
    write16(addr, highWord(val));
    write16(addr + 2, lowWord(val));
}

u16 M68K::peekWord(u32 addr) {
    return bus->read16(addr & 0xFFFFFE);
}

void M68K::invalidatePrefetch() {
    prefetchValid_ = false;
    prefetchWord0_ = 0;
    prefetchWord1_ = 0;
    prefetchPC_ = 0;
    prefetchNextPC_ = 0;
}

void M68K::refillPrefetch() {
    prefetchPC_ = state.pc & 0xFFFFFF;
    prefetchWord0_ = read16(prefetchPC_);
    prefetchWord1_ = read16((prefetchPC_ + 2) & 0xFFFFFF);
    prefetchNextPC_ = (prefetchPC_ + 4) & 0xFFFFFF;
    prefetchValid_ = true;
}

u16 M68K::fetchWord() {
    const u32 pc = state.pc & 0xFFFFFF;
    if (!prefetchValid_ || prefetchPC_ != pc) {
        refillPrefetch();
    }

    const u16 val = prefetchWord0_;
    prefetchWord0_ = prefetchWord1_;
    prefetchWord1_ = read16(prefetchNextPC_);
    prefetchPC_ = (prefetchPC_ + 2) & 0xFFFFFF;
    prefetchNextPC_ = (prefetchNextPC_ + 2) & 0xFFFFFF;
    state.pc = (state.pc + 2) & 0xFFFFFF;
    return val;
}

u32 M68K::fetchLong() {
    u32 val = read32(state.pc);
    state.pc += 4;
    return val;
}

void M68K::push16(u16 val) {
    state.a[7] -= 2;
    write16(state.a[7], val);
}

void M68K::push32(u32 val) {
    state.a[7] -= 4;
    write32(state.a[7], val);
}

u16 M68K::pop16() {
    u16 val = read16(state.a[7]);
    state.a[7] += 2;
    return val;
}

u32 M68K::pop32() {
    u32 val = read32(state.a[7]);
    state.a[7] += 4;
    return val;
}

void M68K::setFlag(u16 flag, bool set) {
    if (set) state.sr |= flag;
    else state.sr &= ~flag;
}

bool M68K::getFlag(u16 flag) {
    return (state.sr & flag) != 0;
}

int M68K::getIntMask() {
    return (state.sr >> 8) & 7;
}

void M68K::setIntMask(int level) {
    state.sr = (state.sr & 0xF8FF) | ((level & 7) << 8);
}

bool M68K::inSupervisorMode() {
    return getFlag(FLAG_S);
}

void M68K::setSupervisorMode(bool super) {
    if (super && !inSupervisorMode()) {
        // Entering supervisor mode - save USP
        state.usp = state.a[7];
        state.a[7] = state.ssp;
    } else if (!super && inSupervisorMode()) {
        // Leaving supervisor mode - restore USP
        state.ssp = state.a[7];
        state.a[7] = state.usp;
    }
    setFlag(FLAG_S, super);
}

bool M68K::testCondition(int cc) {
    bool c = getFlag(FLAG_C);
    bool v = getFlag(FLAG_V);
    bool z = getFlag(FLAG_Z);
    bool n = getFlag(FLAG_N);

    switch (cc) {
        case 0x0: return true;              // T (true)
        case 0x1: return false;             // F (false)
        case 0x2: return !c && !z;          // HI (high)
        case 0x3: return c || z;            // LS (low or same)
        case 0x4: return !c;                // CC (carry clear)
        case 0x5: return c;                 // CS (carry set)
        case 0x6: return !z;                // NE (not equal)
        case 0x7: return z;                 // EQ (equal)
        case 0x8: return !v;                // VC (overflow clear)
        case 0x9: return v;                 // VS (overflow set)
        case 0xA: return !n;                // PL (plus)
        case 0xB: return n;                 // MI (minus)
        case 0xC: return (n && v) || (!n && !v);  // GE (greater or equal)
        case 0xD: return (n && !v) || (!n && v);  // LT (less than)
        case 0xE: return (n && v && !z) || (!n && !v && !z);  // GT (greater than)
        case 0xF: return z || (n && !v) || (!n && v);  // LE (less or equal)
    }
    return false;
}

void M68K::interrupt(int level) {
    if (level > 0 && level <= 7) {
        if (state.pendingInterrupt < level) {
            state.pendingInterrupt = level;
        }
    }
}

void M68K::processInterrupt() {
    if (state.pendingInterrupt < 0) return;

    int level = state.pendingInterrupt;
    int mask = getIntMask();

    // Level 7 is NMI, always processed
    if (level > mask || level == 7) {
        if (interruptAckCallback_) {
            interruptAckCallback_(level);
        }

        state.pendingInterrupt = -1;
        state.stopped = false;

        // Vector number: 24 + level (autovector)
        int vector = 24 + level;
        exception(vector);
        // Autovector interrupts take 50-59 total cycles depending on the
        // E-clock phase when the acknowledge cycle starts. Match GPGX's table
        // using our cumulative master-clock phase; exception() already added 34.
        static constexpr int kInterruptCycles[10] = {50, 59, 58, 57, 56,
                                                     55, 54, 53, 52, 51};
        const u64 current68kCycles = masterCycles_ / 7;
        const int totalInterruptCycles =
            kInterruptCycles[current68kCycles % 10];
        state.cycles += totalInterruptCycles - 34;
        setIntMask(level);
    } else {
        // Interrupt blocked - clear pending if mask blocks it
        // Keep it pending so it can fire when mask is lowered
    }
}

void M68K::exception(int vector) {
    // Save current status
    u16 oldSR = state.sr;
    invalidatePrefetch();

    // Enter supervisor mode
    setSupervisorMode(true);

    // Disable trace
    setFlag(FLAG_T, false);

    // Push PC and SR
    push32(state.pc);
    push16(oldSR);

    // Fetch new PC from vector table
    state.pc = read32(vector * 4);

    state.cycles += 34;  // Exception processing time
}

int M68K::execute() {
    if (state.halted) return 4;
    if (bus && bus->isM68KBusStalled()) return 4;

    // Check for pending interrupts
    state.cycles = 0;
    if (state.pendingInterrupt >= 0) {
        processInterrupt();
    }

    if (state.stopped) return state.cycles > 0 ? state.cycles : 4;

    // Save exception processing cycles (e.g. 34 for interrupt acknowledge)
    // so they aren't lost when we reset cycles for the instruction.
    int exceptionCycles = state.cycles;

    // Rebuild prefetch from the current PC each instruction.
    // This keeps execution deterministic when external code mutates memory
    // between execute() calls (e.g., test harnesses and debugger pokes).
    invalidatePrefetch();

    // Fetch instruction
    lastPC_ = state.pc & 0xFFFFFF;
    state.ir = fetchWord();
    lastIR_ = state.ir;
    state.cycles = exceptionCycles;

    // 68K bus refresh delay (Mega Drive specific): DRAM refresh steals
    // 2 master clocks every 128 master clocks.  Only active when connected
    // to a bus (skipped in test harnesses that run bare instructions).
    if (refreshEnabled_) {
        u64 currentMclk = masterCycles_ + state.cycles * 7;
        if (currentMclk >= refreshNextMclk_) {
            refreshNextMclk_ = currentMclk + 128 * 7;
            state.cycles += 2;
        }
    }

    // Remember trace flag before instruction (instruction may clear it)
    bool traceWasSet = (state.sr & FLAG_T) != 0;

    // Execute
    executeInstruction();

    // Trace exception: if trace was set before this instruction, generate vector 9
    if (traceWasSet) {
        exception(9);
    }

    // Minimum 4 cycles
    if (state.cycles < 4) state.cycles = 4;

    // Instruction trace for timing comparison with Musashi
    {
        static int traceCount = -2;
        if (traceCount == -2) {
            const char* e = std::getenv("M68K_TRACE_INSN");
            traceCount = e ? std::atoi(e) : 0;
        }
        if (traceCount > 0) {
            std::fprintf(stderr, "[INSN] pc=%06X ir=%04X cyc=%d\n",
                         lastPC_, lastIR_, state.cycles);
            traceCount--;
        }
    }

    totalCycles_ += state.cycles;
    masterCycles_ += state.cycles * 7;
    return state.cycles;
}

void M68K::executeInstruction() {
    int group = (state.ir >> 12) & 0xF;

    switch (group) {
        case 0x0: group0(); break;
        case 0x1: group1(); break;
        case 0x2: group2(); break;
        case 0x3: group3(); break;
        case 0x4: group4(); break;
        case 0x5: group5(); break;
        case 0x6: group6(); break;
        case 0x7: group7(); break;
        case 0x8: group8(); break;
        case 0x9: group9(); break;
        case 0xA: groupA(); break;
        case 0xB: groupB(); break;
        case 0xC: groupC(); break;
        case 0xD: groupD(); break;
        case 0xE: groupE(); break;
        case 0xF: groupF(); break;
    }
}

u32 M68K::getEA(int mode, int reg, int size) {
    switch (mode) {
        case 0: // Dn - Data register direct
            return 0;  // Not an address
        case 1: // An - Address register direct
            return 0;  // Not an address
        case 2: // (An) - Address register indirect
            return state.a[reg];
        case 3: { // (An)+ - Post-increment
            u32 addr = state.a[reg];
            int inc = (size == SIZE_BYTE && reg != 7) ? 1 : (size == SIZE_BYTE ? 2 : (size == SIZE_WORD ? 2 : 4));
            state.a[reg] += inc;
            return addr;
        }
        case 4: { // -(An) - Pre-decrement
            int dec = (size == SIZE_BYTE && reg != 7) ? 1 : (size == SIZE_BYTE ? 2 : (size == SIZE_WORD ? 2 : 4));
            state.a[reg] -= dec;
            return state.a[reg];
        }
        case 5: { // d16(An) - Displacement
            s16 disp = static_cast<s16>(fetchWord());
            return state.a[reg] + disp;
        }
        case 6: { // d8(An,Xn) - Index
            u16 ext = fetchWord();
            int xreg = (ext >> 12) & 7;
            bool isAddr = (ext >> 15) & 1;
            bool isLong = (ext >> 11) & 1;
            s8 disp = ext & 0xFF;

            s32 xval;
            if (isAddr) {
                xval = isLong ? static_cast<s32>(state.a[xreg]) : static_cast<s16>(state.a[xreg]);
            } else {
                xval = isLong ? static_cast<s32>(state.d[xreg]) : static_cast<s16>(state.d[xreg]);
            }
            return state.a[reg] + disp + xval;
        }
        case 7:
            switch (reg) {
                case 0: { // xxxx.W - Absolute short
                    s16 addr = static_cast<s16>(fetchWord());
                    return static_cast<u32>(static_cast<s32>(addr));
                }
                case 1: { // xxxxxxxx.L - Absolute long
                    return fetchLong();
                }
                case 2: { // d16(PC) - PC displacement
                    u32 pcBase = state.pc;
                    s16 disp = static_cast<s16>(fetchWord());
                    return pcBase + disp;
                }
                case 3: { // d8(PC,Xn) - PC index
                    u32 pcBase = state.pc;
                    u16 ext = fetchWord();
                    int xreg = (ext >> 12) & 7;
                    bool isAddr = (ext >> 15) & 1;
                    bool isLong = (ext >> 11) & 1;
                    s8 disp = ext & 0xFF;

                    s32 xval;
                    if (isAddr) {
                        xval = isLong ? static_cast<s32>(state.a[xreg]) : static_cast<s16>(state.a[xreg]);
                    } else {
                        xval = isLong ? static_cast<s32>(state.d[xreg]) : static_cast<s16>(state.d[xreg]);
                    }
                    return pcBase + disp + xval;
                }
                case 4: // #imm - Immediate
                    return 0;  // Handled separately
            }
            break;
    }
    return 0;
}

u32 M68K::readEA(int mode, int reg, int size) {
    switch (mode) {
        case 0: // Dn
            return state.d[reg] & maskResult(0xFFFFFFFF, size);
        case 1: // An
            return state.a[reg] & maskResult(0xFFFFFFFF, size);
        case 7:
            if (reg == 4) { // Immediate
                if (size == SIZE_LONG) return fetchLong();
                return fetchWord() & maskResult(0xFFFFFFFF, size);
            }
            // Fall through for other mode 7 cases
            [[fallthrough]];
        default: {
            const int partialCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
            u32 addr = getEA(mode, reg, size);
            switch (size) {
                case SIZE_BYTE: return read8Timed(addr, partialCycles);
                case SIZE_WORD: return read16Timed(addr, partialCycles);
                case SIZE_LONG: return read32Timed(addr, partialCycles);
            }
        }
    }
    return 0;
}

void M68K::writeEA(int mode, int reg, int size, u32 val) {
    switch (mode) {
        case 0: // Dn
            switch (size) {
                case SIZE_BYTE:
                    state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (val & 0xFF);
                    break;
                case SIZE_WORD:
                    state.d[reg] = (state.d[reg] & 0xFFFF0000) | (val & 0xFFFF);
                    break;
                case SIZE_LONG:
                    state.d[reg] = val;
                    break;
            }
            return;
        case 1: // An
            if (size == SIZE_WORD) {
                state.a[reg] = signExtend(val, SIZE_WORD);
            } else {
                state.a[reg] = val;
            }
            return;
        default: {
            u32 addr = getEA(mode, reg, size);
            switch (size) {
                case SIZE_BYTE: write8(addr, val); break;
                case SIZE_WORD: write16(addr, val); break;
                case SIZE_LONG: write32(addr, val); break;
            }
        }
    }
}

u32 M68K::readMemSized(u32 addr, int size) {
    switch (size) {
        case SIZE_BYTE: return read8(addr);
        case SIZE_WORD: return read16(addr);
        case SIZE_LONG: return read32(addr);
        default: return read16(addr);
    }
}

void M68K::writeMemSized(u32 addr, int size, u32 val) {
    switch (size) {
        case SIZE_BYTE: write8(addr, val); break;
        case SIZE_WORD: write16(addr, val); break;
        case SIZE_LONG: write32(addr, val); break;
        default: write16(addr, val); break;
    }
}

int M68K::eaReadAccessCycles(int mode, int reg, int size) {
    switch (mode) {
        case 0: // Dn
        case 1: // An
            return 0;
        case 2: // (An)
        case 3: // (An)+
        case 4: // -(An)
            return 4;
        case 5: // d16(An)
            return 8;
        case 6: // d8(An,Xn)
            return 10;
        case 7:
            switch (reg) {
                case 0: return 8;   // xxx.W
                case 1: return 12;  // xxx.L
                case 2: return 8;   // d16(PC)
                case 3: return 10;  // d8(PC,Xn)
                case 4: return (size == SIZE_LONG) ? 8 : 4;  // #imm data word(s)
                default: return 0;
            }
        default:
            return 0;
    }
}

int M68K::eaCycles(int mode, int reg, int size) {
    bool isLong = (size == SIZE_LONG);
    switch (mode) {
        case 0: return 0;                          // Dn
        case 1: return 0;                          // An
        case 2: return isLong ? 8 : 4;             // (An)
        case 3: return isLong ? 8 : 4;             // (An)+
        case 4: return isLong ? 10 : 6;            // -(An)
        case 5: return isLong ? 12 : 8;            // d(An)
        case 6: return isLong ? 14 : 10;           // d(An,Xi)
        case 7:
            switch (reg) {
                case 0: return isLong ? 12 : 8;    // xxx.W
                case 1: return isLong ? 16 : 12;   // xxx.L
                case 2: return isLong ? 12 : 8;    // d(PC)
                case 3: return isLong ? 14 : 10;   // d(PC,Xi)
                case 4: return isLong ? 8 : 4;     // #imm
            }
            break;
    }
    return 0;
}

int M68K::eaWriteCycles(int mode, int reg, int size) {
    bool isLong = (size == SIZE_LONG);
    switch (mode) {
        case 0: return 0;                          // Dn
        case 1: return 0;                          // An
        case 2: return isLong ? 8 : 4;             // (An)
        case 3: return isLong ? 8 : 4;             // (An)+
        case 4: return isLong ? 8 : 4;             // -(An)
        case 5: return isLong ? 12 : 8;            // d(An)
        case 6: return isLong ? 14 : 10;           // d(An,Xi)
        case 7:
            switch (reg) {
                case 0: return isLong ? 12 : 8;    // xxx.W
                case 1: return isLong ? 16 : 12;   // xxx.L
            }
            break;
    }
    return 0;
}

u32 M68K::signExtend(u32 val, int fromSize) {
    switch (fromSize) {
        case SIZE_BYTE:
            return static_cast<u32>(static_cast<s32>(static_cast<s8>(val)));
        case SIZE_WORD:
            return static_cast<u32>(static_cast<s32>(static_cast<s16>(val)));
        case SIZE_LONG:
            return val;
    }
    return val;
}

u32 M68K::maskResult(u32 val, int size) {
    switch (size) {
        case SIZE_BYTE: return val & 0xFF;
        case SIZE_WORD: return val & 0xFFFF;
        case SIZE_LONG: return val;
    }
    return val;
}

bool M68K::isNegative(u32 val, int size) {
    switch (size) {
        case SIZE_BYTE: return (val & 0x80) != 0;
        case SIZE_WORD: return (val & 0x8000) != 0;
        case SIZE_LONG: return (val & 0x80000000) != 0;
    }
    return false;
}

void M68K::setLogicFlags(u32 result, int size) {
    result = maskResult(result, size);
    setFlag(FLAG_N, isNegative(result, size));
    setFlag(FLAG_Z, result == 0);
    setFlag(FLAG_V, false);
    setFlag(FLAG_C, false);
}

u32 M68K::doAdd(u32 src, u32 dst, int size, bool withX) {
    u64 result;
    u32 mask = maskResult(0xFFFFFFFF, size);
    src &= mask;
    dst &= mask;

    result = static_cast<u64>(src) + static_cast<u64>(dst);
    if (withX && getFlag(FLAG_X)) result++;

    u32 res = result & mask;

    // Carry
    bool carry = (result > mask);
    setFlag(FLAG_C, carry);
    setFlag(FLAG_X, carry);

    // Overflow
    u32 signBit = (size == SIZE_BYTE) ? 0x80 : (size == SIZE_WORD) ? 0x8000 : 0x80000000;
    bool srcSign = (src & signBit) != 0;
    bool dstSign = (dst & signBit) != 0;
    bool resSign = (res & signBit) != 0;
    setFlag(FLAG_V, (srcSign == dstSign) && (resSign != srcSign));

    // Zero (for ADDX, only clear if result is non-zero)
    if (withX) {
        if (res != 0) setFlag(FLAG_Z, false);
    } else {
        setFlag(FLAG_Z, res == 0);
    }

    setFlag(FLAG_N, resSign);

    return res;
}

u32 M68K::doSub(u32 src, u32 dst, int size, bool withX) {
    u64 result;
    u32 mask = maskResult(0xFFFFFFFF, size);
    src &= mask;
    dst &= mask;

    result = static_cast<u64>(dst) - static_cast<u64>(src);
    if (withX && getFlag(FLAG_X)) result--;

    u32 res = result & mask;

    // Borrow (carry)
    bool borrow = (dst < src) || (withX && getFlag(FLAG_X) && dst == src);
    setFlag(FLAG_C, borrow);
    setFlag(FLAG_X, borrow);

    // Overflow
    u32 signBit = (size == SIZE_BYTE) ? 0x80 : (size == SIZE_WORD) ? 0x8000 : 0x80000000;
    bool srcSign = (src & signBit) != 0;
    bool dstSign = (dst & signBit) != 0;
    bool resSign = (res & signBit) != 0;
    setFlag(FLAG_V, (srcSign != dstSign) && (resSign == srcSign));

    // Zero (for SUBX, only clear if result is non-zero)
    if (withX) {
        if (res != 0) setFlag(FLAG_Z, false);
    } else {
        setFlag(FLAG_Z, res == 0);
    }

    setFlag(FLAG_N, resSign);

    return res;
}
