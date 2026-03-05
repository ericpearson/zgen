// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

#ifdef _MSC_VER
#include <intrin.h>
static inline int popcount32(unsigned x) { return __popcnt(x); }
#else
static inline int popcount32(unsigned x) { return __builtin_popcount(x); }
#endif

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
        state.cycles += (size == SIZE_LONG) ? 8 : 4;
        state.cycles += eaCycles(mode, srcReg, size);
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = doAdd(src, dst, size);
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, srcReg, size);
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
    state.cycles += (size == SIZE_LONG) ? 8 : 8;
    state.cycles += eaCycles(mode, srcReg, size);
}

void M68K::op_addi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
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
        u32 dst = readMemSized(addr, size);
        u32 result = doAdd(imm, dst, size);
        writeMemSized(addr, size, result);
    }

    state.cycles += (size == SIZE_LONG) ? 16 : 8;
    if (mode != 0) state.cycles += eaCycles(mode, reg, size);
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
        u32 dst = readMemSized(addr, size);
        u32 result = doAdd(data, dst, size);
        writeMemSized(addr, size, result);
        state.cycles += eaCycles(mode, reg, size);
    }

    // PRM Table 8-2: ADDQ Dn b/w=4, Dn l=8, An=8, mem b/w=8+EA, mem l=12+EA
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 8 : 4;
    } else if (mode == 1) {
        state.cycles += 8;
    } else {
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
    }
}

void M68K::op_addx() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool memMode = (state.ir >> 3) & 1;
    
    if (memMode) {
        int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
        state.a[ry] -= inc;
        state.a[rx] -= inc;
        
        u32 src, dst;
        switch (size) {
            case SIZE_BYTE: src = read8(state.a[ry]); dst = read8(state.a[rx]); break;
            case SIZE_WORD: src = read16(state.a[ry]); dst = read16(state.a[rx]); break;
            case SIZE_LONG: src = read32(state.a[ry]); dst = read32(state.a[rx]); break;
        }
        
        u32 result = doAdd(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: write8(state.a[rx], result); break;
            case SIZE_WORD: write16(state.a[rx], result); break;
            case SIZE_LONG: write32(state.a[rx], result); break;
        }
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
    
    state.cycles += (size == SIZE_LONG) ? 8 : 4;
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
        state.cycles += (size == SIZE_LONG) ? 8 : 4;
        state.cycles += eaCycles(mode, srcReg, size);
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = doSub(src, dst, size);
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, srcReg, size);
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
    state.cycles += (size == SIZE_LONG) ? 8 : 8;
    state.cycles += eaCycles(mode, srcReg, size);
}

void M68K::op_subi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
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
        u32 dst = readMemSized(addr, size);
        u32 result = doSub(imm, dst, size);
        writeMemSized(addr, size, result);
    }

    state.cycles += (size == SIZE_LONG) ? 16 : 8;
    if (mode != 0) state.cycles += eaCycles(mode, reg, size);
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
        u32 dst = readMemSized(addr, size);
        u32 result = doSub(data, dst, size);
        writeMemSized(addr, size, result);
        state.cycles += eaCycles(mode, reg, size);
    }

    // PRM Table 8-2: SUBQ Dn b/w=4, Dn l=8, An=8, mem b/w=8+EA, mem l=12+EA
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 8 : 4;
    } else if (mode == 1) {
        state.cycles += 8;
    } else {
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
    }
}

void M68K::op_subx() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool memMode = (state.ir >> 3) & 1;
    
    if (memMode) {
        int inc = (size == SIZE_BYTE) ? 1 : (size == SIZE_WORD) ? 2 : 4;
        state.a[ry] -= inc;
        state.a[rx] -= inc;
        
        u32 src, dst;
        switch (size) {
            case SIZE_BYTE: src = read8(state.a[ry]); dst = read8(state.a[rx]); break;
            case SIZE_WORD: src = read16(state.a[ry]); dst = read16(state.a[rx]); break;
            case SIZE_LONG: src = read32(state.a[ry]); dst = read32(state.a[rx]); break;
        }
        
        u32 result = doSub(src, dst, size, true);
        
        switch (size) {
            case SIZE_BYTE: write8(state.a[rx], result); break;
            case SIZE_WORD: write16(state.a[rx], result); break;
            case SIZE_LONG: write32(state.a[rx], result); break;
        }
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
    
    state.cycles += (size == SIZE_LONG) ? 8 : 4;
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
        state.cycles += (size == SIZE_LONG) ? 6 : 4;
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = doSub(dst, 0, size);
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, reg, size);
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
        state.cycles += (size == SIZE_LONG) ? 6 : 4;
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = doSub(dst, 0, size, true);
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_clr() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    writeEA(mode, reg, size, 0);

    setFlag(FLAG_N, false);
    setFlag(FLAG_Z, true);
    setFlag(FLAG_V, false);
    setFlag(FLAG_C, false);

    state.cycles += (size == SIZE_LONG) ? 6 : 4;
    if (mode != 0) state.cycles += eaWriteCycles(mode, reg, size);
}

void M68K::op_cmp() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;

    u32 src = readEA(mode, srcReg, size);
    u32 dst = state.d[reg];

    doSub(src, dst, size);

    state.cycles += (size == SIZE_LONG) ? 6 : 4;
    state.cycles += eaCycles(mode, srcReg, size);
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
    
    doSub(src, state.a[reg], SIZE_LONG);

    state.cycles += 6;
    state.cycles += eaCycles(mode, srcReg, size);
}

void M68K::op_cmpi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
    u32 dst = readEA(mode, reg, size);
    doSub(imm, dst, size);

    state.cycles += (size == SIZE_LONG) ? 14 : 8;
    if (mode != 0) state.cycles += eaCycles(mode, reg, size);
}

void M68K::op_cmpm() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    
    u32 src = readEA(3, ry, size); // (Ay)+
    u32 dst = readEA(3, rx, size); // (Ax)+
    
    doSub(src, dst, size);
    
    state.cycles += (size == SIZE_LONG) ? 20 : 12;
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
    state.cycles += 38 + 2 * n + eaCycles(mode, srcReg, SIZE_WORD);
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
    state.cycles += 38 + 2 * n + eaCycles(mode, srcReg, SIZE_WORD);
}

void M68K::op_divu() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    u16 divisor = readEA(mode, srcReg, SIZE_WORD);
    u32 dividend = state.d[reg];
    
    if (divisor == 0) {
        exception(5); // Division by zero
        state.cycles += 8;  // Total = 42 per PRM (exception() adds 34)
        return;
    }

    u32 quotient = dividend / divisor;
    u16 remainder = dividend % divisor;
    
    if (quotient > 0xFFFF) {
        setFlag(FLAG_V, true);
        setFlag(FLAG_C, false);
        // Overflow: 10 cycles + EA
        state.cycles += 10 + eaCycles(mode, srcReg, SIZE_WORD);
    } else {
        state.d[reg] = (remainder << 16) | (quotient & 0xFFFF);
        setFlag(FLAG_N, (quotient & 0x8000) != 0);
        setFlag(FLAG_Z, quotient == 0);
        setFlag(FLAG_V, false);
        setFlag(FLAG_C, false);
        // Use a data-dependent timing approximation in the documented 76-140 range.
        // This is closer to 68000 DIVU behavior than the old 102-134 estimate.
        int cycles = 76 + (popcount32(static_cast<u16>(quotient)) * 4);
        state.cycles += cycles + eaCycles(mode, srcReg, SIZE_WORD);
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
        state.cycles += 8;  // Total = 42 per PRM (exception() adds 34)
        return;
    }

    s32 quotient = dividend / divisor;
    s16 remainder = dividend % divisor;
    
    if (quotient < -32768 || quotient > 32767) {
        setFlag(FLAG_V, true);
        setFlag(FLAG_C, false);
        // Overflow: 16-18 cycles + EA (use 16 as base)
        state.cycles += 16 + eaCycles(mode, srcReg, SIZE_WORD);
    } else {
        state.d[reg] = (static_cast<u16>(remainder) << 16) | (static_cast<u16>(quotient) & 0xFFFF);
        setFlag(FLAG_N, (quotient & 0x8000) != 0);
        setFlag(FLAG_Z, quotient == 0);
        setFlag(FLAG_V, false);
        setFlag(FLAG_C, false);
        // DIVS timing from Yacht.txt: base 12 + operand-dependent costs.
        // Count quotient bits that differ from dividend sign bit.
        bool dividendNeg = dividend < 0;
        u16 absQ = static_cast<u16>(quotient >= 0 ? quotient : -quotient);
        int n = 0;
        for (int i = 15; i >= 0; i--) {
            bool bit = (absQ & (1 << i)) != 0;
            if (bit != dividendNeg) n++;
        }
        int cycles = 12 + 2 * n;
        // Sign correction: +2 if dividend and divisor signs differ
        if ((dividend < 0) != (divisor < 0)) cycles += 2;
        // Base overhead for sign handling
        cycles += 6;
        // Clamp to documented range [120, 158]
        if (cycles < 120) cycles = 120;
        if (cycles > 158) cycles = 158;
        state.cycles += cycles + eaCycles(mode, srcReg, SIZE_WORD);
    }
}

void M68K::op_nbcd() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    u32 addr = (mode == 0) ? 0 : getEA(mode, reg, SIZE_BYTE);
    u8 dst = (mode == 0) ? (state.d[reg] & 0xFF) : readMemSized(addr, SIZE_BYTE);
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
        writeMemSized(addr, SIZE_BYTE, result);
    }

    setFlag(FLAG_N, (result & 0x80) != 0);
    setFlag(FLAG_V, false);

    state.cycles += (mode == 0) ? 6 : (8 + eaCycles(mode, reg, SIZE_BYTE));
}

void M68K::op_abcd() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    bool memMode = (state.ir >> 3) & 1;
    
    u8 src, dst;
    if (memMode) {
        state.a[ry]--;
        state.a[rx]--;
        src = read8(state.a[ry]);
        dst = read8(state.a[rx]);
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
        write8(state.a[rx], result);
    } else {
        state.d[rx] = (state.d[rx] & 0xFFFFFF00) | result;
    }
    
    state.cycles += memMode ? 18 : 6;
}

void M68K::op_sbcd() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    bool memMode = (state.ir >> 3) & 1;
    
    u8 src, dst;
    if (memMode) {
        state.a[ry]--;
        state.a[rx]--;
        src = read8(state.a[ry]);
        dst = read8(state.a[rx]);
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
        write8(state.a[rx], result);
    } else {
        state.d[rx] = (state.d[rx] & 0xFFFFFF00) | result;
    }
    
    state.cycles += memMode ? 18 : 6;
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
    
    state.cycles += 10 + eaCycles(mode, srcReg, SIZE_WORD);
}
