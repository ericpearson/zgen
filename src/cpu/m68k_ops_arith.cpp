// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

#ifdef _MSC_VER
#include <intrin.h>
static inline int popcount32(unsigned x) { return __popcnt(x); }
#else
static inline int popcount32(unsigned x) { return __builtin_popcount(x); }
#endif

static int predecrementAmount(int size, int reg) {
    if (size == SIZE_BYTE) {
        return reg == 7 ? 2 : 1;
    }
    return size == SIZE_WORD ? 2 : 4;
}

static int divsCoreCycles(u32 dividendIn, u16 divisorWord) {
    // Yacht's DIVS operand-dependent timing loop. Keep the arithmetic result
    // path below separate; this helper models the microcoded cycle count.
    u32 dividend = dividendIn;
    u32 divisorShift = static_cast<u32>(divisorWord) << 16;
    const u32 origDivisor = divisorShift;
    const u32 origDividend = dividend;

    if (divisorShift & 0x80000000u) {
        divisorShift = 0u - divisorShift;
    }

    int cycles = 12;
    if (dividend & 0x80000000u) {
        dividend = 0u - dividend;
        cycles += 2;
    }

    if (divisorShift <= dividend) {
        return cycles + 4;
    }

    u16 quotient = 0;
    u16 bit = 0;
    for (int i = 0; i < 15; i++) {
        quotient = static_cast<u16>((quotient << 1) | bit);
        dividend <<= 1;

        if (dividend >= divisorShift) {
            dividend -= divisorShift;
            cycles += 6;
            bit = 1;
        } else {
            cycles += 8;
            bit = 0;
        }
    }

    quotient = static_cast<u16>((quotient << 1) | bit);
    dividend <<= 1;
    if (dividend >= divisorShift) {
        dividend -= divisorShift;
        quotient = static_cast<u16>((quotient << 1) | 1);
    } else {
        quotient = static_cast<u16>(quotient << 1);
    }
    cycles += 4;

    if (origDivisor & 0x80000000u) {
        cycles += 16;
    } else if (origDividend & 0x80000000u) {
        cycles += 18;
    } else {
        cycles += 14;
    }
    return cycles;
}

static int divuCoreCycles(u32 dividendIn, u16 divisorWord) {
    // Yacht's DIVU operand-dependent timing loop. The arithmetic result path
    // below stays separate; this helper models the microcoded cycle count.
    u32 dividend = dividendIn;
    const u32 divisorShift = static_cast<u32>(divisorWord) << 16;
    u16 quotient = 0;
    u16 bit = 0;
    bool force = false;
    int cycles = 6;

    for (int i = 0; i < 16; i++) {
        force = (dividend & 0x80000000u) != 0;
        quotient = static_cast<u16>((quotient << 1) | bit);
        dividend <<= 1;

        if (force || dividend >= divisorShift) {
            dividend -= divisorShift;
            cycles += force ? 4 : 6;
            bit = 1;
        } else {
            bit = 0;
            cycles += 8;
        }
    }

    cycles += force ? 6 : (bit ? 4 : 2);
    return cycles;
}

// Arithmetic Instructions

void M68K::op_add() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    bool toReg = !((state.ir >> 8) & 1);

    if (toReg) {
        u32 src = readEA(mode, srcReg, size);
        u32 dst = state.d[reg];
        u32 result = doAdd(src, dst, size);

        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
        }
        // yacht.txt: .L with memory source = 6 (ALU overlaps bus read),
        // Dn/An/#imm source = 8
        bool memSrc = (mode >= 2) && !(mode == 7 && srcReg == 4);
        chargeCycles((size == SIZE_LONG) ? (memSrc ? 6 : 8) : 4);
        chargeCycles(eaCycles(mode, srcReg, size));
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, srcReg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doAdd(src, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
        chargeCycles(eaCycles(mode, srcReg, size));
    }
}

void M68K::op_adda() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir & 0x100) ? SIZE_LONG : SIZE_WORD;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u32 src = readEA(mode, srcReg, size);
    if (size == SIZE_WORD) {
        src = signExtend(src, SIZE_WORD);
    }
    
    state.a[reg] += src;
    chargeCycles((size == SIZE_LONG) ? 8 : 8);
    chargeCycles(eaCycles(mode, srcReg, size));
}

void M68K::op_addi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    const int immediateFetchCycles = (size == SIZE_LONG) ? 8 : 4;
    chargeCycles(immediateFetchCycles);
    
    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doAdd(imm, dst, size);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doAdd(imm, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
    }

    // yacht.txt: Dn = 8/16, memory = 12/20
    if (mode == 0) {
        chargeCycles(((size == SIZE_LONG) ? 16 : 8) - immediateFetchCycles);
    } else {
        chargeCycles(((size == SIZE_LONG) ? 20 : 12) - immediateFetchCycles);
        chargeCycles(eaCycles(mode, reg, size));
    }
}

void M68K::op_addq() {
    int data = (state.ir >> 9) & 0x7;
    if (data == 0) data = 8;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    if (mode == 1) {
        // Address register - no flags affected
        state.a[reg] += data;
    } else if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doAdd(data, dst, size);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doAdd(data, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles(eaCycles(mode, reg, size));
    }

    // 68000 timing: ADDQ memory b/w=8+EA, mem l=12+EA.
    if (mode == 0) {
        chargeCycles((size == SIZE_LONG) ? 8 : 4);
    } else if (mode == 1) {
        chargeCycles(8);
    } else {
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
    }
}

void M68K::op_addx() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool memMode = (state.ir >> 3) & 1;
    
    if (memMode) {
        state.a[ry] -= predecrementAmount(size, ry);
        state.a[rx] -= predecrementAmount(size, rx);
        
        u32 src, dst;
        switch (size) {
            case SIZE_BYTE:
                src = read8Timed(state.a[ry], state.cycles + 6);
                dst = read8Timed(state.a[rx], state.cycles + 10);
                break;
            case SIZE_WORD:
                src = read16Timed(state.a[ry], state.cycles + 6);
                dst = read16Timed(state.a[rx], state.cycles + 10);
                break;
            case SIZE_LONG:
                src = read32Timed(state.a[ry], state.cycles + 6);
                dst = read32Timed(state.a[rx], state.cycles + 14);
                break;
        }
        
        u32 result = doAdd(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: write8Timed(state.a[rx], result, state.cycles + 14); break;
            case SIZE_WORD: write16Timed(state.a[rx], result, state.cycles + 14); break;
            case SIZE_LONG: write32Timed(state.a[rx], result, state.cycles + 22); break;
        }

        chargeCycles((size == SIZE_LONG) ? 30 : 18);
        return;
    } else {
        u32 src = state.d[ry] & maskResult(0xFFFFFFFF, size);
        u32 dst = state.d[rx] & maskResult(0xFFFFFFFF, size);
        u32 result = doAdd(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: state.d[rx] = (state.d[rx] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[rx] = (state.d[rx] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[rx] = result; break;
        }
    }
    
    chargeCycles((size == SIZE_LONG) ? 8 : 4);
}

void M68K::op_sub() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    bool toReg = !((state.ir >> 8) & 1);

    if (toReg) {
        u32 src = readEA(mode, srcReg, size);
        u32 dst = state.d[reg];
        u32 result = doSub(src, dst, size);

        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
        }
        // yacht.txt: .L with memory source = 6 (ALU overlaps bus read),
        // Dn/An/#imm source = 8
        bool memSrc = (mode >= 2) && !(mode == 7 && srcReg == 4);
        chargeCycles((size == SIZE_LONG) ? (memSrc ? 6 : 8) : 4);
        chargeCycles(eaCycles(mode, srcReg, size));
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, srcReg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doSub(src, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
        chargeCycles(eaCycles(mode, srcReg, size));
    }
}

void M68K::op_suba() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir & 0x100) ? SIZE_LONG : SIZE_WORD;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u32 src = readEA(mode, srcReg, size);
    if (size == SIZE_WORD) {
        src = signExtend(src, SIZE_WORD);
    }
    
    state.a[reg] -= src;
    chargeCycles((size == SIZE_LONG) ? 8 : 8);
    chargeCycles(eaCycles(mode, srcReg, size));
}

void M68K::op_subi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    const int immediateFetchCycles = (size == SIZE_LONG) ? 8 : 4;
    chargeCycles(immediateFetchCycles);
    
    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doSub(imm, dst, size);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doSub(imm, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
    }

    // yacht.txt: Dn = 8/16, memory = 12/20
    if (mode == 0) {
        chargeCycles(((size == SIZE_LONG) ? 16 : 8) - immediateFetchCycles);
    } else {
        chargeCycles(((size == SIZE_LONG) ? 20 : 12) - immediateFetchCycles);
        chargeCycles(eaCycles(mode, reg, size));
    }
}

void M68K::op_subq() {
    int data = (state.ir >> 9) & 0x7;
    if (data == 0) data = 8;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    if (mode == 1) {
        // Address register - no flags affected
        state.a[reg] -= data;
    } else if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doSub(data, dst, size);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doSub(data, dst, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles(eaCycles(mode, reg, size));
    }

    // 68000 timing: SUBQ memory matches ADDQ.
    if (mode == 0) {
        chargeCycles((size == SIZE_LONG) ? 8 : 4);
    } else if (mode == 1) {
        chargeCycles(8);
    } else {
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
    }
}

void M68K::op_subx() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool memMode = (state.ir >> 3) & 1;
    
    if (memMode) {
        state.a[ry] -= predecrementAmount(size, ry);
        state.a[rx] -= predecrementAmount(size, rx);
        
        u32 src, dst;
        switch (size) {
            case SIZE_BYTE:
                src = read8Timed(state.a[ry], state.cycles + 6);
                dst = read8Timed(state.a[rx], state.cycles + 10);
                break;
            case SIZE_WORD:
                src = read16Timed(state.a[ry], state.cycles + 6);
                dst = read16Timed(state.a[rx], state.cycles + 10);
                break;
            case SIZE_LONG:
                src = read32Timed(state.a[ry], state.cycles + 6);
                dst = read32Timed(state.a[rx], state.cycles + 14);
                break;
        }
        
        u32 result = doSub(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: write8Timed(state.a[rx], result, state.cycles + 14); break;
            case SIZE_WORD: write16Timed(state.a[rx], result, state.cycles + 14); break;
            case SIZE_LONG: write32Timed(state.a[rx], result, state.cycles + 22); break;
        }

        chargeCycles((size == SIZE_LONG) ? 30 : 18);
        return;
    } else {
        u32 src = state.d[ry] & maskResult(0xFFFFFFFF, size);
        u32 dst = state.d[rx] & maskResult(0xFFFFFFFF, size);
        u32 result = doSub(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: state.d[rx] = (state.d[rx] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[rx] = (state.d[rx] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[rx] = result; break;
        }
    }
    
    chargeCycles((size == SIZE_LONG) ? 8 : 4);
}

void M68K::op_neg() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doSub(dst, 0, size);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        chargeCycles((size == SIZE_LONG) ? 6 : 4);
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doSub(dst, 0, size);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
        chargeCycles(eaCycles(mode, reg, size));
    }
}

void M68K::op_negx() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = doSub(dst, 0, size, true);
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        chargeCycles((size == SIZE_LONG) ? 6 : 4);
    } else {
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        u32 dst = readMemSizedTimed(addr, size, readAccessCycles);
        u32 result = doSub(dst, 0, size, true);
        writeMemSizedTimed(addr, size, result, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
        chargeCycles(eaCycles(mode, reg, size));
    }
}

void M68K::op_clr() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    if (mode == 0) {
        switch (size) {
            case SIZE_BYTE: state.d[reg] &= 0xFFFFFF00; break;
            case SIZE_WORD: state.d[reg] &= 0xFFFF0000; break;
            case SIZE_LONG: state.d[reg] = 0; break;
        }
        chargeCycles((size == SIZE_LONG) ? 6 : 4);
    } else {
        // Real 68000 does read-then-write even though result is always zero
        u32 addr = getEA(mode, reg, size);
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, size);
        readMemSizedTimed(addr, size, readAccessCycles);  // dummy read (hardware behavior)
        writeMemSizedTimed(addr, size, 0, readAccessCycles + ((size == SIZE_LONG) ? 8 : 4));
        chargeCycles((size == SIZE_LONG) ? 12 : 8);
        chargeCycles(eaCycles(mode, reg, size));
    }

    setFlag(FLAG_N, false);
    setFlag(FLAG_Z, true);
    setFlag(FLAG_V, false);
    setFlag(FLAG_C, false);
}

void M68K::op_cmp() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;

    u32 src = readEA(mode, srcReg, size);
    u32 dst = state.d[reg];
    const bool oldX = getFlag(FLAG_X);
    doSub(src, dst, size);
    setFlag(FLAG_X, oldX);

    chargeCycles((size == SIZE_LONG) ? 6 : 4);
    chargeCycles(eaCycles(mode, srcReg, size));
}

void M68K::op_cmpa() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir & 0x100) ? SIZE_LONG : SIZE_WORD;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u32 src = readEA(mode, srcReg, size);
    if (size == SIZE_WORD) {
        src = signExtend(src, SIZE_WORD);
    }
    
    const bool oldX = getFlag(FLAG_X);
    doSub(src, state.a[reg], SIZE_LONG);
    setFlag(FLAG_X, oldX);

    chargeCycles(6);
    chargeCycles(eaCycles(mode, srcReg, size));
}

void M68K::op_cmpi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;

    const int immediateFetchCycles = (size == SIZE_LONG) ? 8 : 4;
    chargeCycles(immediateFetchCycles);
    
    u32 dst = readEA(mode, reg, size);
    const bool oldX = getFlag(FLAG_X);
    doSub(imm, dst, size);
    setFlag(FLAG_X, oldX);

    chargeCycles(((size == SIZE_LONG) ? 14 : 8) - immediateFetchCycles);
    if (mode != 0) chargeCycles(eaCycles(mode, reg, size));
}

void M68K::op_cmpm() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    
    u32 src = readEA(3, ry, size); // (Ay)+
    u32 dst = readEA(3, rx, size); // (Ax)+
    
    const bool oldX = getFlag(FLAG_X);
    doSub(src, dst, size);
    setFlag(FLAG_X, oldX);
    
    chargeCycles((size == SIZE_LONG) ? 20 : 12);
}

void M68K::op_mulu() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u16 src = readEA(mode, srcReg, SIZE_WORD);
    u16 dst = state.d[reg] & 0xFFFF;
    
    u32 result = static_cast<u32>(src) * static_cast<u32>(dst);
    state.d[reg] = result;
    
    setLogicFlags(result, SIZE_LONG);
    // Real 68000: 38 + 2n cycles where n = number of 1-bits in source operand
    int n = popcount32(src);
    chargeCycles(38 + 2 * n + eaCycles(mode, srcReg, SIZE_WORD));
}

void M68K::op_muls() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;

    s16 src = static_cast<s16>(readEA(mode, srcReg, SIZE_WORD));
    s16 dst = static_cast<s16>(state.d[reg] & 0xFFFF);

    s32 result = static_cast<s32>(src) * static_cast<s32>(dst);
    state.d[reg] = static_cast<u32>(result);

    setLogicFlags(state.d[reg], SIZE_LONG);
    // Real 68000: 38 + 2n cycles where n = number of 01/10 bit-pair transitions
    u16 v = static_cast<u16>(src);
    u16 transitions = (v ^ (v << 1)) & 0xFFFF;
    int n = popcount32(transitions);
    chargeCycles(38 + 2 * n + eaCycles(mode, srcReg, SIZE_WORD));
}

void M68K::op_divu() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u16 divisor = readEA(mode, srcReg, SIZE_WORD);
    u32 dividend = state.d[reg];
    
    if (divisor == 0) {
        exception(5); // Division by zero
        chargeCycles(8);  // Total = 42 per PRM (exception() adds 34)
        return;
    }

    u32 quotient = dividend / divisor;
    u16 remainder = dividend % divisor;
    
    if (quotient > 0xFFFF) {
        setFlag(FLAG_V, true);
        setFlag(FLAG_C, false);
        // Overflow: 10 cycles + EA
        chargeCycles(10 + eaCycles(mode, srcReg, SIZE_WORD));
    } else {
        state.d[reg] = (remainder << 16) | (quotient & 0xFFFF);
        setFlag(FLAG_N, (quotient & 0x8000) != 0);
        setFlag(FLAG_Z, quotient == 0);
        setFlag(FLAG_V, false);
        setFlag(FLAG_C, false);
        const int cycles = divuCoreCycles(dividend, divisor);
        chargeCycles(cycles + eaCycles(mode, srcReg, SIZE_WORD));
    }
}

void M68K::op_divs() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    s16 divisor = static_cast<s16>(readEA(mode, srcReg, SIZE_WORD));
    s32 dividend = static_cast<s32>(state.d[reg]);
    
    if (divisor == 0) {
        exception(5);
        chargeCycles(8);  // Total = 42 per PRM (exception() adds 34)
        return;
    }

    const int cycles = divsCoreCycles(static_cast<u32>(dividend), static_cast<u16>(divisor));
    const s64 quotientWide = static_cast<s64>(dividend) / static_cast<s64>(divisor);
    const s16 remainder = static_cast<s16>(static_cast<s64>(dividend) % static_cast<s64>(divisor));
    
    if (quotientWide < -32768 || quotientWide > 32767) {
        setFlag(FLAG_V, true);
        setFlag(FLAG_C, false);
        chargeCycles(cycles + eaCycles(mode, srcReg, SIZE_WORD));
    } else {
        const s32 quotient = static_cast<s32>(quotientWide);
        state.d[reg] = (static_cast<u16>(remainder) << 16) | (static_cast<u16>(quotient) & 0xFFFF);
        setFlag(FLAG_N, (quotient & 0x8000) != 0);
        setFlag(FLAG_Z, quotient == 0);
        setFlag(FLAG_V, false);
        setFlag(FLAG_C, false);
        chargeCycles(cycles + eaCycles(mode, srcReg, SIZE_WORD));
    }
}

void M68K::op_nbcd() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    u32 addr = 0;
    int readAccessCycles = state.cycles;
    u8 dst;
    if (mode == 0) {
        dst = state.d[reg] & 0xFF;
    } else {
        addr = getEA(mode, reg, SIZE_BYTE);
        readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_BYTE);
        dst = readMemSizedTimed(addr, SIZE_BYTE, readAccessCycles);
    }
    int x = getFlag(FLAG_X) ? 1 : 0;

    // NBCD is decimal subtraction with an implicit zero source:
    // result = 0 - dst - X, performed on packed BCD nibbles.
    int lowNibble = -(dst & 0x0F) - x;
    int highNibble = -((dst >> 4) & 0x0F);

    if (lowNibble < 0) {
        lowNibble += 10;
        highNibble--;
    }

    bool borrow = false;
    if (highNibble < 0) {
        highNibble += 10;
        borrow = true;
    }

    setFlag(FLAG_C, borrow);
    setFlag(FLAG_X, borrow);

    u8 result = ((highNibble & 0xF) << 4) | (lowNibble & 0xF);
    if (result != 0) {
        setFlag(FLAG_Z, false);
    }

    if (mode == 0) {
        state.d[reg] = (state.d[reg] & 0xFFFFFF00) | result;
    } else {
        writeMemSizedTimed(addr, SIZE_BYTE, result, readAccessCycles + 4);
    }

    setFlag(FLAG_N, (result & 0x80) != 0);
    setFlag(FLAG_V, false);

    chargeCycles((mode == 0) ? 6 : (8 + eaCycles(mode, reg, SIZE_BYTE)));
}

void M68K::op_abcd() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    bool memMode = (state.ir >> 3) & 1;
    
    u8 src, dst;
    if (memMode) {
        state.a[ry] -= predecrementAmount(SIZE_BYTE, ry);
        state.a[rx] -= predecrementAmount(SIZE_BYTE, rx);
        src = read8Timed(state.a[ry], state.cycles + 6);
        dst = read8Timed(state.a[rx], state.cycles + 10);
    } else {
        src = state.d[ry] & 0xFF;
        dst = state.d[rx] & 0xFF;
    }
    
    int x = getFlag(FLAG_X) ? 1 : 0;
    int lowNibble = (src & 0x0F) + (dst & 0x0F) + x;
    int highNibble = ((src >> 4) & 0x0F) + ((dst >> 4) & 0x0F);
    
    if (lowNibble > 9) {
        lowNibble -= 10;
        highNibble++;
    }
    
    bool carry = false;
    if (highNibble > 9) {
        highNibble -= 10;
        carry = true;
    }
    
    setFlag(FLAG_C, carry);
    setFlag(FLAG_X, carry);
    
    u8 result = ((highNibble & 0xF) << 4) | (lowNibble & 0xF);
    if (result != 0) setFlag(FLAG_Z, false);
    
    if (memMode) {
        write8Timed(state.a[rx], result, state.cycles + 14);
    } else {
        state.d[rx] = (state.d[rx] & 0xFFFFFF00) | result;
    }
    
    chargeCycles(memMode ? 18 : 6);
}

void M68K::op_sbcd() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    bool memMode = (state.ir >> 3) & 1;
    
    u8 src, dst;
    if (memMode) {
        state.a[ry] -= predecrementAmount(SIZE_BYTE, ry);
        state.a[rx] -= predecrementAmount(SIZE_BYTE, rx);
        src = read8Timed(state.a[ry], state.cycles + 6);
        dst = read8Timed(state.a[rx], state.cycles + 10);
    } else {
        src = state.d[ry] & 0xFF;
        dst = state.d[rx] & 0xFF;
    }
    
    int x = getFlag(FLAG_X) ? 1 : 0;
    int lowNibble = (dst & 0x0F) - (src & 0x0F) - x;
    int highNibble = ((dst >> 4) & 0x0F) - ((src >> 4) & 0x0F);
    
    if (lowNibble < 0) {
        lowNibble += 10;
        highNibble--;
    }
    
    bool borrow = false;
    if (highNibble < 0) {
        highNibble += 10;
        borrow = true;
    }
    
    setFlag(FLAG_C, borrow);
    setFlag(FLAG_X, borrow);
    
    u8 result = ((highNibble & 0xF) << 4) | (lowNibble & 0xF);
    if (result != 0) setFlag(FLAG_Z, false);
    
    if (memMode) {
        write8Timed(state.a[rx], result, state.cycles + 14);
    } else {
        state.d[rx] = (state.d[rx] & 0xFFFFFF00) | result;
    }
    
    chargeCycles(memMode ? 18 : 6);
}

void M68K::op_chk() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    s16 bound = static_cast<s16>(readEA(mode, srcReg, SIZE_WORD));
    s16 val = static_cast<s16>(state.d[reg] & 0xFFFF);
    
    if (val < 0) {
        setFlag(FLAG_N, true);
        exception(6);
    } else if (val > bound) {
        setFlag(FLAG_N, false);
        exception(6);
    }
    
    chargeCycles(10 + eaCycles(mode, srcReg, SIZE_WORD));
}
