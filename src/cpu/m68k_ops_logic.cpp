// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

// Logical Instructions

void M68K::op_and() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    bool toReg = !((state.ir >> 8) & 1);

    u32 result;
    if (toReg) {
        u32 src = readEA(mode, srcReg, size);
        result = state.d[reg] & src;

        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
        }
        // yacht.txt: .L with memory source = 6 (ALU overlaps bus read),
        // Dn/An/#imm source = 8
        bool memSrc = (mode >= 2) && !(mode == 7 && srcReg == 4);
        state.cycles += (size == SIZE_LONG) ? (memSrc ? 6 : 8) : 4;
        state.cycles += eaCycles(mode, srcReg, size);
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        u32 dst = readMemSized(addr, size);
        result = src & dst;
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, srcReg, size);
    }

    setLogicFlags(result, size);
}

void M68K::op_andi() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    // Special case: ANDI to CCR/SR
    if (mode == 7 && reg == 4) {
        u16 imm = fetchWord();
        if (size == SIZE_BYTE) {
            state.sr &= (0xFF00 | (imm & 0x1F));
        } else {
            if (!inSupervisorMode()) {
                exception(8);
                return;
            }
            state.sr &= (imm & 0xA71F);  // Mask to valid 68000 bits
        }
        state.cycles += 20;
        return;
    }
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = dst & imm;
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        setLogicFlags(result, size);
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = dst & imm;
        writeMemSized(addr, size, result);
        setLogicFlags(result, size);
    }
    // yacht.txt: Dn = 8/16, memory = 12/20
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 16 : 8;
    } else {
        state.cycles += (size == SIZE_LONG) ? 20 : 12;
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_or() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    bool toReg = !((state.ir >> 8) & 1);

    u32 result;
    if (toReg) {
        u32 src = readEA(mode, srcReg, size);
        result = state.d[reg] | src;

        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
        }
        // yacht.txt: .L with memory source = 6 (ALU overlaps bus read),
        // Dn/An/#imm source = 8
        bool memSrc = (mode >= 2) && !(mode == 7 && srcReg == 4);
        state.cycles += (size == SIZE_LONG) ? (memSrc ? 6 : 8) : 4;
        state.cycles += eaCycles(mode, srcReg, size);
    } else {
        u32 src = state.d[reg];
        u32 addr = getEA(mode, srcReg, size);
        u32 dst = readMemSized(addr, size);
        result = src | dst;
        writeMemSized(addr, size, result);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, srcReg, size);
    }

    setLogicFlags(result, size);
}

void M68K::op_ori() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    // Special case: ORI to CCR/SR
    if (mode == 7 && reg == 4) {
        u16 imm = fetchWord();
        if (size == SIZE_BYTE) {
            state.sr |= (imm & 0x1F);
        } else {
            if (!inSupervisorMode()) {
                exception(8);
                return;
            }
            state.sr |= (imm & 0xA71F);  // Mask to valid 68000 bits
        }
        state.cycles += 20;
        return;
    }
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = dst | imm;
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        setLogicFlags(result, size);
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = dst | imm;
        writeMemSized(addr, size, result);
        setLogicFlags(result, size);
    }
    // yacht.txt: Dn = 8/16, memory = 12/20
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 16 : 8;
    } else {
        state.cycles += (size == SIZE_LONG) ? 20 : 12;
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_eor() {
    int reg = (state.ir >> 9) & 0x7;
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int dstReg = state.ir & 0x7;
    
    u32 src = state.d[reg];
    u32 result;
    if (mode == 0) {
        u32 dst = state.d[dstReg] & maskResult(0xFFFFFFFF, size);
        result = src ^ dst;
        switch (size) {
            case SIZE_BYTE: state.d[dstReg] = (state.d[dstReg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[dstReg] = (state.d[dstReg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[dstReg] = result; break;
            default: state.d[dstReg] = (state.d[dstReg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
    } else {
        u32 addr = getEA(mode, dstReg, size);
        u32 dst = readMemSized(addr, size);
        result = src ^ dst;
        writeMemSized(addr, size, result);
    }
    
    setLogicFlags(result, size);
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 8 : 4;
    } else {
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, dstReg, size);
    }
}

void M68K::op_eori() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    // Special case: EORI to CCR/SR
    if (mode == 7 && reg == 4) {
        u16 imm = fetchWord();
        if (size == SIZE_BYTE) {
            state.sr ^= (imm & 0x1F);
        } else {
            if (!inSupervisorMode()) {
                exception(8);
                return;
            }
            state.sr ^= (imm & 0xA71F);  // Mask to valid 68000 bits
        }
        state.cycles += 20;
        return;
    }
    
    u32 imm = (size == SIZE_LONG) ? fetchLong() : fetchWord();
    if (size == SIZE_BYTE) imm &= 0xFF;
    
    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = dst ^ imm;
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        setLogicFlags(result, size);
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = dst ^ imm;
        writeMemSized(addr, size, result);
        setLogicFlags(result, size);
    }
    // yacht.txt: Dn = 8/16, memory = 12/20
    if (mode == 0) {
        state.cycles += (size == SIZE_LONG) ? 16 : 8;
    } else {
        state.cycles += (size == SIZE_LONG) ? 20 : 12;
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_not() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    if (mode == 0) {
        u32 dst = state.d[reg] & maskResult(0xFFFFFFFF, size);
        u32 result = ~dst;
        switch (size) {
            case SIZE_BYTE: state.d[reg] = (state.d[reg] & 0xFFFFFF00) | (result & 0xFF); break;
            case SIZE_WORD: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
            case SIZE_LONG: state.d[reg] = result; break;
            default: state.d[reg] = (state.d[reg] & 0xFFFF0000) | (result & 0xFFFF); break;
        }
        setLogicFlags(result, size);
        state.cycles += (size == SIZE_LONG) ? 6 : 4;
    } else {
        u32 addr = getEA(mode, reg, size);
        u32 dst = readMemSized(addr, size);
        u32 result = ~dst;
        writeMemSized(addr, size, result);
        setLogicFlags(result, size);
        state.cycles += (size == SIZE_LONG) ? 12 : 8;
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_tst() {
    int size = (state.ir >> 6) & 0x3;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    u32 val = readEA(mode, reg, size);
    setLogicFlags(val, size);

    // PRM Table 8-2: TST = 4(1/0) + EA for all modes
    state.cycles += 4;
    if (mode != 0) {
        state.cycles += eaCycles(mode, reg, size);
    }
}

void M68K::op_tas() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u32 addr = (mode == 0) ? 0 : getEA(mode, reg, SIZE_BYTE);
    u8 val = (mode == 0) ? (state.d[reg] & 0xFF) : readMemSized(addr, SIZE_BYTE);
    setLogicFlags(val, SIZE_BYTE);
    
    // Set high bit
    if (mode == 0) {
        state.d[reg] = (state.d[reg] & 0xFFFFFF00) | ((val | 0x80) & 0xFF);
    }
    // Memory write-back suppressed on real Genesis hardware:
    // The 68000 cannot lock the external bus for TAS read-modify-write,
    // so the write cycle never completes. Games like Gargoyles depend on this.
    
    state.cycles += (mode == 0) ? 4 : 14;
}

// Bit manipulation instructions

void M68K::op_btst() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    int bitNum;
    
    if (state.ir & 0x100) {
        // Dynamic - bit number in register
        int bitReg = (state.ir >> 9) & 0x7;
        bitNum = state.d[bitReg];
    } else {
        // Static - bit number in extension word
        bitNum = fetchWord() & 0xFF;
    }
    
    // PRM: dynamic Dn=6, static Dn=10, dynamic mem=4+EA, static mem=8+EA
    bool isStatic = !(state.ir & 0x100);
    if (mode == 0) {
        bitNum &= 31;
        setFlag(FLAG_Z, (state.d[reg] & (1 << bitNum)) == 0);
        state.cycles += isStatic ? 10 : 6;
    } else {
        bitNum &= 7;
        u8 val = readEA(mode, reg, SIZE_BYTE);
        setFlag(FLAG_Z, (val & (1 << bitNum)) == 0);
        state.cycles += (isStatic ? 8 : 4) + eaCycles(mode, reg, SIZE_BYTE);
    }
}

void M68K::op_bchg() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    int bitNum;
    
    if (state.ir & 0x100) {
        int bitReg = (state.ir >> 9) & 0x7;
        bitNum = state.d[bitReg];
    } else {
        bitNum = fetchWord() & 0xFF;
    }
    
    // yacht.txt: Dn bit<16=6, bit>=16=8; static bit<16=10, bit>=16=12
    bool isStatic = !(state.ir & 0x100);
    if (mode == 0) {
        bitNum &= 31;
        setFlag(FLAG_Z, (state.d[reg] & (1 << bitNum)) == 0);
        state.d[reg] ^= (1 << bitNum);
        if (isStatic) {
            state.cycles += (bitNum < 16) ? 10 : 12;
        } else {
            state.cycles += (bitNum < 16) ? 6 : 8;
        }
    } else {
        bitNum &= 7;
        u32 addr = getEA(mode, reg, SIZE_BYTE);
        u8 val = readMemSized(addr, SIZE_BYTE);
        setFlag(FLAG_Z, (val & (1 << bitNum)) == 0);
        writeMemSized(addr, SIZE_BYTE, val ^ (1 << bitNum));
        state.cycles += (isStatic ? 12 : 8) + eaCycles(mode, reg, SIZE_BYTE);
    }
}

void M68K::op_bclr() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    int bitNum;
    
    if (state.ir & 0x100) {
        int bitReg = (state.ir >> 9) & 0x7;
        bitNum = state.d[bitReg];
    } else {
        bitNum = fetchWord() & 0xFF;
    }
    
    // yacht.txt: Dn bit<16=8, bit>=16=10; static bit<16=12, bit>=16=14
    bool isStatic = !(state.ir & 0x100);
    if (mode == 0) {
        bitNum &= 31;
        setFlag(FLAG_Z, (state.d[reg] & (1 << bitNum)) == 0);
        state.d[reg] &= ~(1 << bitNum);
        if (isStatic) {
            state.cycles += (bitNum < 16) ? 12 : 14;
        } else {
            state.cycles += (bitNum < 16) ? 8 : 10;
        }
    } else {
        bitNum &= 7;
        u32 addr = getEA(mode, reg, SIZE_BYTE);
        u8 val = readMemSized(addr, SIZE_BYTE);
        setFlag(FLAG_Z, (val & (1 << bitNum)) == 0);
        writeMemSized(addr, SIZE_BYTE, val & ~(1 << bitNum));
        state.cycles += (isStatic ? 12 : 8) + eaCycles(mode, reg, SIZE_BYTE);
    }
}

void M68K::op_bset() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    int bitNum;
    
    if (state.ir & 0x100) {
        int bitReg = (state.ir >> 9) & 0x7;
        bitNum = state.d[bitReg];
    } else {
        bitNum = fetchWord() & 0xFF;
    }
    
    // yacht.txt: Dn bit<16=6, bit>=16=8; static bit<16=10, bit>=16=12
    bool isStatic = !(state.ir & 0x100);
    if (mode == 0) {
        bitNum &= 31;
        setFlag(FLAG_Z, (state.d[reg] & (1 << bitNum)) == 0);
        state.d[reg] |= (1 << bitNum);
        if (isStatic) {
            state.cycles += (bitNum < 16) ? 10 : 12;
        } else {
            state.cycles += (bitNum < 16) ? 6 : 8;
        }
    } else {
        bitNum &= 7;
        u32 addr = getEA(mode, reg, SIZE_BYTE);
        u8 val = readMemSized(addr, SIZE_BYTE);
        setFlag(FLAG_Z, (val & (1 << bitNum)) == 0);
        writeMemSized(addr, SIZE_BYTE, val | (1 << bitNum));
        state.cycles += (isStatic ? 12 : 8) + eaCycles(mode, reg, SIZE_BYTE);
    }
}
