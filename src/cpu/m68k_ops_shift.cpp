// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

// Shift and Rotate Instructions

void M68K::op_asl_asr_reg() {
    int count;
    int reg = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool left = (state.ir >> 8) & 1;
    
    if (state.ir & 0x20) {
        // Count in register
        int countReg = (state.ir >> 9) & 0x7;
        count = state.d[countReg] & 63;
    } else {
        // Immediate count
        count = (state.ir >> 9) & 0x7;
        if (count == 0) count = 8;
    }
    
    u32 val = state.d[reg];
    u32 mask = maskResult(0xFFFFFFFF, size);
    val &= mask;
    
    bool lastBit = false;
    bool overflow = false;
    
    for (int i = 0; i < count; i++) {
        if (left) {
            lastBit = isNegative(val, size);
            val <<= 1;
            if (isNegative(val, size) != lastBit) overflow = true;
        } else {
            lastBit = val & 1;
            if (size == SIZE_BYTE) {
                val = static_cast<u32>(static_cast<s8>(val) >> 1) & 0xFF;
            } else if (size == SIZE_WORD) {
                val = static_cast<u32>(static_cast<s16>(val) >> 1) & 0xFFFF;
            } else {
                val = static_cast<u32>(static_cast<s32>(val) >> 1);
            }
        }
    }
    
    val &= mask;
    
    if (count > 0) {
        setFlag(FLAG_C, lastBit);
        setFlag(FLAG_X, lastBit);
    } else {
        setFlag(FLAG_C, false);
    }
    
    setFlag(FLAG_V, overflow);
    setFlag(FLAG_N, isNegative(val, size));
    setFlag(FLAG_Z, val == 0);
    
    switch (size) {
        case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | val; break;
        case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | val; break;
        case SIZE_LONG: state.d[reg] = val; break;
    }

    chargeCycles(((size == SIZE_LONG) ? 8 : 6) + 2 * count);
}

void M68K::op_asl_asr_mem() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    bool left = (state.ir >> 8) & 1;

    u32 addr = getEA(mode, reg, SIZE_WORD);
    const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_WORD);
    u16 val = readMemSizedTimed(addr, SIZE_WORD, readAccessCycles);
    bool lastBit;
    bool overflow = false;

    if (left) {
        lastBit = (val & 0x8000) != 0;
        bool newSign = (val & 0x4000) != 0;
        val <<= 1;
        overflow = (lastBit != newSign);
    } else {
        lastBit = val & 1;
        val = static_cast<u16>(static_cast<s16>(val) >> 1);
    }

    writeMemSizedTimed(addr, SIZE_WORD, val, readAccessCycles + 4);

    setFlag(FLAG_C, lastBit);
    setFlag(FLAG_X, lastBit);
    setFlag(FLAG_V, overflow);
    setFlag(FLAG_N, (val & 0x8000) != 0);
    setFlag(FLAG_Z, val == 0);

    chargeCycles(8 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_lsl_lsr_reg() {
    int count;
    int reg = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool left = (state.ir >> 8) & 1;
    
    if (state.ir & 0x20) {
        int countReg = (state.ir >> 9) & 0x7;
        count = state.d[countReg] & 63;
    } else {
        count = (state.ir >> 9) & 0x7;
        if (count == 0) count = 8;
    }
    
    u32 val = state.d[reg];
    u32 mask = maskResult(0xFFFFFFFF, size);
    val &= mask;
    
    bool lastBit = false;
    
    for (int i = 0; i < count; i++) {
        if (left) {
            lastBit = isNegative(val, size);
            val <<= 1;
        } else {
            lastBit = val & 1;
            val >>= 1;
        }
    }
    
    val &= mask;
    
    if (count > 0) {
        setFlag(FLAG_C, lastBit);
        setFlag(FLAG_X, lastBit);
    } else {
        setFlag(FLAG_C, false);
    }
    
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, isNegative(val, size));
    setFlag(FLAG_Z, val == 0);
    
    switch (size) {
        case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | val; break;
        case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | val; break;
        case SIZE_LONG: state.d[reg] = val; break;
    }

    chargeCycles(((size == SIZE_LONG) ? 8 : 6) + 2 * count);
}

void M68K::op_lsl_lsr_mem() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    bool left = (state.ir >> 8) & 1;

    u32 addr = getEA(mode, reg, SIZE_WORD);
    const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_WORD);
    u16 val = readMemSizedTimed(addr, SIZE_WORD, readAccessCycles);
    bool lastBit;

    if (left) {
        lastBit = (val & 0x8000) != 0;
        val <<= 1;
    } else {
        lastBit = val & 1;
        val >>= 1;
    }

    writeMemSizedTimed(addr, SIZE_WORD, val, readAccessCycles + 4);

    setFlag(FLAG_C, lastBit);
    setFlag(FLAG_X, lastBit);
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, (val & 0x8000) != 0);
    setFlag(FLAG_Z, val == 0);

    chargeCycles(8 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_rol_ror_reg() {
    int count;
    int reg = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool left = (state.ir >> 8) & 1;
    
    if (state.ir & 0x20) {
        int countReg = (state.ir >> 9) & 0x7;
        count = state.d[countReg] & 63;
    } else {
        count = (state.ir >> 9) & 0x7;
        if (count == 0) count = 8;
    }
    
    u32 val = state.d[reg];
    u32 mask = maskResult(0xFFFFFFFF, size);
    val &= mask;
    
    int bits = (size == SIZE_BYTE) ? 8 : (size == SIZE_WORD) ? 16 : 32;
    count %= bits;
    
    bool lastBit = false;
    
    if (count > 0) {
        if (left) {
            lastBit = (val >> (bits - 1)) & 1;
            val = ((val << count) | (val >> (bits - count))) & mask;
            lastBit = val & 1;
        } else {
            lastBit = val & 1;
            val = ((val >> count) | (val << (bits - count))) & mask;
            lastBit = isNegative(val, size);
        }
    }
    
    if (count > 0) {
        setFlag(FLAG_C, lastBit);
    } else {
        setFlag(FLAG_C, false);
    }
    
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, isNegative(val, size));
    setFlag(FLAG_Z, val == 0);
    
    switch (size) {
        case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | val; break;
        case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | val; break;
        case SIZE_LONG: state.d[reg] = val; break;
    }

    chargeCycles(((size == SIZE_LONG) ? 8 : 6) + 2 * count);
}

void M68K::op_rol_ror_mem() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    bool left = (state.ir >> 8) & 1;

    u32 addr = getEA(mode, reg, SIZE_WORD);
    const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_WORD);
    u16 val = readMemSizedTimed(addr, SIZE_WORD, readAccessCycles);
    bool lastBit;

    if (left) {
        lastBit = (val & 0x8000) != 0;
        val = (val << 1) | (lastBit ? 1 : 0);
    } else {
        lastBit = val & 1;
        val = (val >> 1) | (lastBit ? 0x8000 : 0);
    }

    writeMemSizedTimed(addr, SIZE_WORD, val, readAccessCycles + 4);

    setFlag(FLAG_C, lastBit);
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, (val & 0x8000) != 0);
    setFlag(FLAG_Z, val == 0);

    chargeCycles(8 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_roxl_roxr_reg() {
    int count;
    int reg = state.ir & 0x7;
    int size = (state.ir >> 6) & 0x3;
    bool left = (state.ir >> 8) & 1;
    
    if (state.ir & 0x20) {
        int countReg = (state.ir >> 9) & 0x7;
        count = state.d[countReg] & 63;
    } else {
        count = (state.ir >> 9) & 0x7;
        if (count == 0) count = 8;
    }
    
    u32 val = state.d[reg];
    u32 mask = maskResult(0xFFFFFFFF, size);
    val &= mask;
    
    bool x = getFlag(FLAG_X);
    
    for (int i = 0; i < count; i++) {
        bool oldX = x;
        if (left) {
            x = isNegative(val, size);
            val = ((val << 1) | (oldX ? 1 : 0)) & mask;
        } else {
            x = val & 1;
            u32 highBit = oldX ? (1 << ((size == SIZE_BYTE) ? 7 : (size == SIZE_WORD) ? 15 : 31)) : 0;
            val = (val >> 1) | highBit;
        }
    }
    
    setFlag(FLAG_X, x);
    setFlag(FLAG_C, x);
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, isNegative(val, size));
    setFlag(FLAG_Z, val == 0);
    
    switch (size) {
        case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | val; break;
        case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | val; break;
        case SIZE_LONG: state.d[reg] = val; break;
    }

    chargeCycles(((size == SIZE_LONG) ? 8 : 6) + 2 * count);
}

void M68K::op_roxl_roxr_mem() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    bool left = (state.ir >> 8) & 1;

    u32 addr = getEA(mode, reg, SIZE_WORD);
    const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_WORD);
    u16 val = readMemSizedTimed(addr, SIZE_WORD, readAccessCycles);
    bool x = getFlag(FLAG_X);
    bool newX;

    if (left) {
        newX = (val & 0x8000) != 0;
        val = (val << 1) | (x ? 1 : 0);
    } else {
        newX = val & 1;
        val = (val >> 1) | (x ? 0x8000 : 0);
    }

    writeMemSizedTimed(addr, SIZE_WORD, val, readAccessCycles + 4);

    setFlag(FLAG_X, newX);
    setFlag(FLAG_C, newX);
    setFlag(FLAG_V, false);
    setFlag(FLAG_N, (val & 0x8000) != 0);
    setFlag(FLAG_Z, val == 0);

    chargeCycles(8 + eaCycles(mode, reg, SIZE_WORD));
}
