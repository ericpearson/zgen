// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"
#include "memory/bus.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
inline bool fifoTimingLogEnabled() {
    static int e = -1;
    if (e < 0) { const char* v = std::getenv("GENESIS_LOG_FIFO_TIMING"); e = (v && std::atoi(v)) ? 1 : 0; }
    return e != 0;
}

inline bool cottonSeqLogEnabled() {
    static int e = -1;
    if (e < 0) {
        const char* v = std::getenv("GENESIS_LOG_COTTON_SEQ");
        e = (v && v[0] != '0') ? 1 : 0;
    }
    return e != 0;
}

inline bool cottonSeqLogFrameEnabled(int frame) {
    static int frameFirst = -1;
    static int frameLast = -1;
    static bool initialized = false;
    if (!initialized) {
        const char* ff = std::getenv("GENESIS_LOG_COTTON_FRAME_FIRST");
        const char* fl = std::getenv("GENESIS_LOG_COTTON_FRAME_LAST");
        frameFirst = ff ? std::atoi(ff) : -1;
        frameLast = fl ? std::atoi(fl) : -1;
        initialized = true;
    }
    if (frameFirst < 0 && frameLast < 0) {
        return true;
    }
    const int last = (frameLast >= 0) ? frameLast : frameFirst;
    return frame >= frameFirst && frame <= last;
}

inline bool isCottonWatchPc(u32 pc) {
    switch (pc & 0xFFFFFF) {
        case 0x000C48:
        case 0x004E56:
        case 0x004EA4:
        case 0x004EB0:
        case 0x004EB6:
        case 0x004EBC:
        case 0x0262EC:
        case 0x0265CC:
        case 0x030A8A:
        case 0x0022C0:
        case 0x002286:
        case 0x00228C:
        case 0x002292:
        case 0x0022F4:
        case 0x0022DE:
        case 0x00233A:
        case 0x002342:
        case 0x00234E:
        case 0x00235A:
        case 0x002360:
        case 0x002368:
        case 0x00236A:
        case 0x002376:
        case 0x002380:
        case 0x0023A2:
        case 0x0023AA:
        case 0x0023B4:
        case 0x0023F8:
        case 0x002402:
        case 0x002408:
        case 0x0025A8:
        case 0x0025BE:
        case 0x0025C4:
        case 0x0025CE:
        case 0x0025D2:
        case 0x0025E2:
        case 0x0025D8:
        case 0x0025DA:
        case 0x002600:
        case 0x002620:
        case 0x00263E:
        case 0x00266E:
        case 0x002676:
        case 0x00267A:
        case 0x002680:
        case 0x0026B6:
        case 0x00270A:
        case 0x002712:
        case 0x00271A:
        case 0x002720:
        case 0x002722:
        case 0x002724:
        case 0x002726:
        case 0x002728:
        case 0x00272A:
        case 0x00272C:
        case 0x00272E:
        case 0x002730:
        case 0x002732:
        case 0x002734:
        case 0x002736:
        case 0x002738:
        case 0x005690:
        case 0x00569C:
        case 0x0056B0:
        case 0x0056BA:
            return true;
        default:
            return false;
    }
}
} // namespace

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
    masterCycles_ = 0;
    totalCycles_ = 0;

    // Read initial SSP and PC from vector table
    state.ssp = read32(0x000000);
    state.pc = read32(0x000004);
    state.a[7] = state.ssp;

    // Start in supervisor mode with interrupts masked
    state.sr = FLAG_S | (7 << 8);
    state.stopped = false;
    state.halted = false;

    // Reset exception overhead: 40 M68K cycles (280 master clocks).
    // Per M68K hardware reference for the reset exception sequence.
    totalCycles_ = 40;
    masterCycles_ = 40 * 7;
    wallClockMclks_ = masterCycles_;
    refreshCounter_ = 0;
    refreshLastSync_ = masterCycles_;
}

int M68K::applyRefreshFreeAccess() {
    if (!refreshEnabled_) return 0;
    // Phase 3: refresh penalty is now charged inline in advanceClock(). The
    // IO-site call only advances the counter for the "current 4-MCLK access"
    // representing bus-access time; it must NOT charge penalty separately
    // (that would double-count). refreshLastSync_ is owned by advanceClock.
    refreshCounter_ = static_cast<u32>(
        (static_cast<u64>(refreshCounter_) + 4 * 7) % kRefreshInterval);
    return 0;
}

void M68K::advanceRefreshNoWait(int m68kCycles) {
    if (!refreshEnabled_ || m68kCycles <= 0) return;
    u64 elapsed = masterCycles_ - refreshLastSync_;
    refreshLastSync_ = masterCycles_;
    refreshCounter_ = static_cast<u32>(
        (static_cast<u64>(refreshCounter_) + elapsed) % kRefreshInterval);
}

u8 M68K::read8(u32 addr) {
    return bus->read8(addr & 0xFFFFFF);
}

u16 M68K::read16(u32 addr) {
    if (addr & 1) {
        // Address error: vector 3, 50 cycles per PRM (exception() adds 34)
        exception(3);
        chargeCycles(16);
    }
    return bus->read16(addr & 0xFFFFFE);
}

u8 M68K::read8Timed(u32 addr, int partialCycles) {
    return bus->read8(addr & 0xFFFFFF, partialCycles);
}

u16 M68K::read16Timed(u32 addr, int partialCycles) {
    if (addr & 1) {
        exception(3);
        chargeCycles(16);
    }
    return bus->read16(addr & 0xFFFFFE, partialCycles);
}

u32 M68K::read32Timed(u32 addr, int partialCycles) {
    u16 high = read16Timed(addr, partialCycles);
    u16 low = read16Timed(addr + 2, partialCycles + 4);
    return makeLong(high, low);
}

void M68K::write8Timed(u32 addr, u8 val, int partialCycles) {
    if (fifoTimingLogEnabled() && (addr & 0xFFFFFF) >= 0xC00000 && (addr & 0xFFFFFF) < 0xC00020) {
        std::fprintf(stderr, "[W-TIMED8] addr=%06X val=%02X eaPartialCyc=%d instrCyc=%d eaOffset=%d\n",
                     addr & 0xFFFFFF, val, partialCycles, state.cycles, partialCycles - state.cycles);
    }
    bus->write8(addr & 0xFFFFFF, val, partialCycles);
}

void M68K::write16Timed(u32 addr, u16 val, int partialCycles) {
    if (fifoTimingLogEnabled() && (addr & 0xFFFFFE) >= 0xC00000 && (addr & 0xFFFFFE) < 0xC00020) {
        std::fprintf(stderr, "[W-TIMED16] addr=%06X val=%04X eaPartialCyc=%d instrCyc=%d eaOffset=%d\n",
                     addr & 0xFFFFFE, val, partialCycles, state.cycles, partialCycles - state.cycles);
    }
    if (addr & 1) {
        // Address error: 50 cycles per PRM (exception() adds 34)
        exception(3);
        chargeCycles(16);
    }
    bus->write16(addr & 0xFFFFFE, val, partialCycles);
}

void M68K::write32Timed(u32 addr, u32 val, int partialCycles) {
    write16Timed(addr, highWord(val), partialCycles);
    write16Timed(addr + 2, lowWord(val), partialCycles + 4);
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
        chargeCycles(16);
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
        // The base 68000 autovector path is 44 cycles total, but on Genesis
        // the IACK bus cycle uses !VPA and behaves like a slow 6800
        // peripheral acknowledge. Hardware-facing references model this as
        // an additional variable 5-14 cycle delay based on the current 68K phase.
        // Keep the old fixed 44-cycle path unless the diagnostic env is set,
        // so timing investigations can toggle the behavior without baking in
        // an unverified global change.
        static int slowIackDiag = -1;
        if (slowIackDiag < 0) {
            const char* env = std::getenv("GENESIS_SLOW_IACK_DIAG");
            slowIackDiag = (env && env[0] != '0') ? 1 : 0;
        }
        if (slowIackDiag) {
            const int iackDelay = 5 + static_cast<int>(totalCycles_ % 10);
            chargeCycles((44 - 34) + iackDelay);
        } else {
            // Autovector interrupt total: 44 cycles (per M68K programmer's
            // reference). exception() already added 34; add the
            // remaining 10 for the interrupt acknowledge bus cycle.
            chargeCycles(44 - 34);
        }
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

    chargeCycles(34);  // Exception processing time
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

    // Instruction trace for timing comparison
    {
        static int traceCount = -2;
        static int traceFrameFirst = 0;
        static int traceFrameLast = 0;
        static int traceRegs = -1;
        if (traceCount == -2) {
            const char* e = std::getenv("M68K_TRACE_INSN");
            traceCount = e ? std::atoi(e) : 0;
            const char* ff = std::getenv("M68K_TRACE_FRAME_FIRST");
            const char* fl = std::getenv("M68K_TRACE_FRAME_LAST");
            traceFrameFirst = ff ? std::atoi(ff) : 0;
            traceFrameLast = fl ? std::atoi(fl) : 0;
            const char* tr = std::getenv("M68K_TRACE_REGS");
            traceRegs = (tr && tr[0] != '0') ? 1 : 0;
        }
        bool frameOk = (traceFrameFirst == 0 && traceFrameLast == 0) ||
                        (traceFrameNumber_ >= traceFrameFirst && traceFrameNumber_ <= traceFrameLast);
        if (traceCount > 0 && frameOk) {
            if (traceRegs) {
                const u16 ff8e04 = read16(0xFF8E04);
                const u8 ff8e05 = read8(0xFF8E05);
                const u16 ff9fa4 = read16(0xFF9FA4);
                const u16 ff9fba = read16(0xFF9FBA);
                const u8 fff463 = read8(0xFFF463);
                const u8 fff465 = read8(0xFFF465);
                const u8 fff466 = read8(0xFFF466);
                std::fprintf(stderr,
                             "[INSN] tc=%llu frame=%d ln=%d pc=%06X ir=%04X cyc=%d "
                             "d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X d5=%08X d6=%08X d7=%08X "
                             "a0=%08X a1=%08X a2=%08X a3=%08X a4=%08X a5=%08X a6=%08X a7=%08X sr=%04X "
                             "ff8e04=%04X ff8e05=%02X ff9fa4=%04X ff9fba=%04X "
                             "fff463=%02X fff465=%02X fff466=%02X\n",
                             static_cast<unsigned long long>(totalCycles_),
                             traceFrameNumber_, traceScanline_,
                             lastPC_, lastIR_, state.cycles,
                             state.d[0], state.d[1], state.d[2], state.d[3],
                             state.d[4], state.d[5], state.d[6], state.d[7],
                             state.a[0], state.a[1], state.a[2], state.a[3],
                             state.a[4], state.a[5], state.a[6], state.a[7],
                             state.sr,
                             ff8e04, ff8e05, ff9fa4, ff9fba,
                             fff463, fff465, fff466);
            } else {
                std::fprintf(stderr, "[INSN] tc=%llu frame=%d ln=%d pc=%06X ir=%04X cyc=%d\n",
                             static_cast<unsigned long long>(totalCycles_),
                             traceFrameNumber_, traceScanline_,
                             lastPC_, lastIR_, state.cycles);
            }
            traceCount--;
        }
    }

    if (cottonSeqLogEnabled() &&
        cottonSeqLogFrameEnabled(traceFrameNumber_) &&
        isCottonWatchPc(lastPC_)) {
        u32 sp = state.a[7];
        u32 ret = bus->debugPeek32(sp & 0xFFFFFF);
        std::fprintf(stderr,
                     "[COTTON-SEQ] tc=%llu frame=%d ln=%d pc=%06X ir=%04X cyc=%d "
                     "d0=%08X d1=%08X d2=%08X d3=%08X a0=%08X a1=%08X a6=%08X a7=%08X ret=%06X sr=%04X "
                     "m[a0]=%04X m[a0+2]=%04X m[a1]=%04X "
                     "m[a6-4]=%04X m[a6-2]=%04X m[a6]=%04X m[a6+2]=%04X "
                     "ff8802=%06X ff8808=%06X ff8812=%06X ff9fa4=%04X ff9fba=%04X ff95a8=%04X ff9604=%04X "
                     "fff2fe=%04X fff300=%04X fff302=%04X fff304=%06X "
                     "ff8e10=%06X ff8e14=%06X ff8e18=%06X ff8e1c=%06X ff8e20=%04X "
                     "fff463=%02X fff466=%02X ff8e04=%04X ff8e05=%02X\n",
                     static_cast<unsigned long long>(totalCycles_),
                     traceFrameNumber_,
                     traceScanline_,
                     lastPC_,
                     lastIR_,
                     state.cycles,
                     state.d[0],
                     state.d[1],
                     state.d[2],
                     state.d[3],
                     state.a[0],
                     state.a[1],
                     state.a[6],
                     sp,
                     ret & 0xFFFFFF,
                     state.sr,
                     bus->debugPeek16(state.a[0] & 0xFFFFFF),
                     bus->debugPeek16((state.a[0] + 2) & 0xFFFFFF),
                     bus->debugPeek16(state.a[1] & 0xFFFFFF),
                     bus->debugPeek16((state.a[6] - 4) & 0xFFFFFF),
                     bus->debugPeek16((state.a[6] - 2) & 0xFFFFFF),
                     bus->debugPeek16(state.a[6] & 0xFFFFFF),
                     bus->debugPeek16((state.a[6] + 2) & 0xFFFFFF),
                     bus->debugPeek32(0xFF8802) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8808) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8812) & 0xFFFFFF,
                     bus->debugPeek16(0xFF9FA4),
                     bus->debugPeek16(0xFF9FBA),
                     bus->debugPeek16(0xFF95A8),
                     bus->debugPeek16(0xFF9604),
                     bus->debugPeek16(0xFFF2FE),
                     bus->debugPeek16(0xFFF300),
                     bus->debugPeek16(0xFFF302),
                     bus->debugPeek32(0xFFF304) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8E10) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8E14) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8E18) & 0xFFFFFF,
                     bus->debugPeek32(0xFF8E1C) & 0xFFFFFF,
                     bus->debugPeek16(0xFF8E20),
                     bus->debugPeek8(0xFFF463),
                     bus->debugPeek8(0xFFF466),
                     bus->debugPeek16(0xFF8E04),
                     bus->debugPeek8(0xFF8E05));
    }

    totalCycles_ += state.cycles;
    masterCycles_ += state.cycles * 7;
    // Phase 2 transition: force-sync the wall-clock to masterCycles_ at the
    // end of every instruction. Migrated ops (calling chargeCycles) will
    // have already advanced wallClockMclks_ inline; unmigrated ops (still
    // doing the legacy assignment directly) catch up here. Once Phase 2 is
    // complete and every op family uses chargeCycles, this can be replaced
    // with a strict equality assertion. Then Phase 3 changes advanceClock
    // to charge refresh inline, breaking the equality intentionally.
    wallClockMclks_ = masterCycles_;
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
            const int partialCycles = state.cycles + eaWriteAccessCycles(mode, reg, size);
            u32 addr = getEA(mode, reg, size);
            switch (size) {
                case SIZE_BYTE: write8Timed(addr, val, partialCycles); break;
                case SIZE_WORD: write16Timed(addr, val, partialCycles); break;
                case SIZE_LONG:
                    if (mode == 4) {
                        write16Timed(addr + 2, lowWord(val), partialCycles);
                        write16Timed(addr, highWord(val), partialCycles + 4);
                    } else {
                        write32Timed(addr, val, partialCycles);
                    }
                    break;
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

u32 M68K::readMemSizedTimed(u32 addr, int size, int partialCycles) {
    switch (size) {
        case SIZE_BYTE: return read8Timed(addr, partialCycles);
        case SIZE_WORD: return read16Timed(addr, partialCycles);
        case SIZE_LONG: return read32Timed(addr, partialCycles);
        default: return read16Timed(addr, partialCycles);
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

void M68K::writeMemSizedTimed(u32 addr, int size, u32 val, int partialCycles) {
    switch (size) {
        case SIZE_BYTE: write8Timed(addr, val, partialCycles); break;
        case SIZE_WORD: write16Timed(addr, val, partialCycles); break;
        case SIZE_LONG: write32Timed(addr, val, partialCycles); break;
        default: write16Timed(addr, val, partialCycles); break;
    }
}

int M68K::eaReadAccessCycles(int mode, int reg, int size) {
    switch (mode) {
        case 0: // Dn
        case 1: // An
            return 0;
        case 2: // (An)
        case 3: // (An)+
            return 4;
        case 4: // -(An)
            return 6;
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

int M68K::eaWriteAccessCycles(int mode, int reg, int size) {
    (void)size;
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
                default: return 0;
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
