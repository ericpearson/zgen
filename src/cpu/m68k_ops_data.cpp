// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

// Data Movement Instructions

void M68K::op_move() {
    int size;
    int group = (state.ir >> 12) & 0x3;
    switch (group) {
        case 1: size = SIZE_BYTE; break;
        case 2: size = SIZE_LONG; break;
        case 3: size = SIZE_WORD; break;
        default: size = SIZE_WORD; break;
    }
    
    int srcMode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    int dstReg = (state.ir >> 9) & 0x7;
    int dstMode = (state.ir >> 6) & 0x7;
    u32 data = readEA(srcMode, srcReg, size);
    const int srcCycles = eaCycles(srcMode, srcReg, size);

    setLogicFlags(data, size);
    chargeCycles(srcCycles);
    writeEA(dstMode, dstReg, size, data);

    chargeCycles(4 + eaWriteCycles(dstMode, dstReg, size));
}

void M68K::op_movea() {
    int size = ((state.ir >> 12) & 0x3) == 3 ? SIZE_WORD : SIZE_LONG;
    int srcMode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    int dstReg = (state.ir >> 9) & 0x7;
    
    u32 data = readEA(srcMode, srcReg, size);
    
    if (size == SIZE_WORD) {
        data = signExtend(data, SIZE_WORD);
    }
    
    state.a[dstReg] = data;
    chargeCycles(4 + eaCycles(srcMode, srcReg, size));
}

void M68K::op_moveq() {
    int reg = (state.ir >> 9) & 0x7;
    s8 data = state.ir & 0xFF;
    state.d[reg] = signExtend(data, SIZE_BYTE);
    
    setLogicFlags(state.d[reg], SIZE_LONG);
    chargeCycles(4);
}

void M68K::op_movep() {
    // MOVEP - Move Peripheral Data
    // Transfers data between a data register and alternating bytes in memory
    // Used for 8-bit peripheral communication on 16-bit bus
    
    int dataReg = (state.ir >> 9) & 0x7;
    int addrReg = state.ir & 0x7;
    int opmode = (state.ir >> 6) & 0x3;
    
    s16 disp = static_cast<s16>(fetchWord());
    u32 addr = state.a[addrReg] + disp;
    int accessCycles = state.cycles + 8;
    
    switch (opmode) {
        case 0: // MOVEP.W (d16,An),Dn - memory to register, word
            {
                u16 val = (read8Timed(addr, accessCycles) << 8) |
                          read8Timed(addr + 2, accessCycles + 4);
                state.d[dataReg] = (state.d[dataReg] & 0xFFFF0000) | val;
            }
            chargeCycles(16);
            break;
            
        case 1: // MOVEP.L (d16,An),Dn - memory to register, long
            {
                u32 val = (read8Timed(addr, accessCycles) << 24) |
                          (read8Timed(addr + 2, accessCycles + 4) << 16) |
                          (read8Timed(addr + 4, accessCycles + 8) << 8) |
                          read8Timed(addr + 6, accessCycles + 12);
                state.d[dataReg] = val;
            }
            chargeCycles(24);
            break;
            
        case 2: // MOVEP.W Dn,(d16,An) - register to memory, word
            write8Timed(addr, (state.d[dataReg] >> 8) & 0xFF, accessCycles);
            write8Timed(addr + 2, state.d[dataReg] & 0xFF, accessCycles + 4);
            chargeCycles(16);
            break;
            
        case 3: // MOVEP.L Dn,(d16,An) - register to memory, long
            write8Timed(addr, (state.d[dataReg] >> 24) & 0xFF, accessCycles);
            write8Timed(addr + 2, (state.d[dataReg] >> 16) & 0xFF, accessCycles + 4);
            write8Timed(addr + 4, (state.d[dataReg] >> 8) & 0xFF, accessCycles + 8);
            write8Timed(addr + 6, state.d[dataReg] & 0xFF, accessCycles + 12);
            chargeCycles(24);
            break;
    }
}

void M68K::op_move_from_sr() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    if (mode == 0) {
        writeEA(mode, reg, SIZE_WORD, state.sr);
    } else {
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_WORD);
        u32 addr = getEA(mode, reg, SIZE_WORD);
        read16Timed(addr, readAccessCycles);
        write16Timed(addr, state.sr, readAccessCycles + 4);
    }
    // PRM: MOVE from SR: Dn=6, mem=8+EA
    chargeCycles((mode == 0) ? 6 : 8 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_move_to_sr() {
    if (!inSupervisorMode()) {
        exception(8); // Privilege violation
        return;
    }
    
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u16 newSR = readEA(mode, reg, SIZE_WORD);
    // Mask to valid 68000 SR bits: T1 S I2-I0 X N Z V C
    // Bits 15, 13, 10-8, 4-0 are valid; others should be 0
    state.sr = newSR & 0xA71F;
    
    // Handle mode switch
    if (!(state.sr & FLAG_S)) {
        // Switching to user mode
        state.ssp = state.a[7];
        state.a[7] = state.usp;
    }
    
    // PRM: MOVE to SR = 12 + EA
    chargeCycles(12 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_move_to_ccr() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u16 data = readEA(mode, reg, SIZE_WORD);
    // Only modify CCR bits (low 5 bits)
    state.sr = (state.sr & 0xFF00) | (data & 0x1F);
    // PRM: MOVE to CCR = 12 + EA
    chargeCycles(12 + eaCycles(mode, reg, SIZE_WORD));
}

void M68K::op_movem() {
    bool toMem = !((state.ir >> 10) & 1);
    int size = (state.ir & 0x40) ? SIZE_LONG : SIZE_WORD;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u16 mask = fetchWord();
    int inc = (size == SIZE_LONG) ? 4 : 2;
    
    // For predecrement/postincrement, don't use getEA as it would modify the register
    u32 addr;
    if (mode == 4) {
        // Predecrement - start at current address, will be decremented in loop
        addr = state.a[reg];
    } else if (mode == 3) {
        // Postincrement - start at current address, will be updated after
        addr = state.a[reg];
    } else {
        addr = getEA(mode, reg, size);
    }

    int accessCycles = state.cycles + eaReadAccessCycles(mode, reg, size) + 4;
    
    if (toMem) {
        if (mode == 4) {
            // Predecrement - reversed order, registers pushed in reverse
            for (int i = 15; i >= 0; i--) {
                if (mask & (1 << (15 - i))) {
                    addr -= inc;
                    u32 val = (i < 8) ? state.d[i] : state.a[i - 8];
                    if (size == SIZE_LONG) {
                        write16Timed(addr + 2, lowWord(val), accessCycles);
                        write16Timed(addr, highWord(val), accessCycles + 4);
                        accessCycles += 8;
                    } else {
                        write16Timed(addr, val, accessCycles);
                        accessCycles += 4;
                    }
                }
            }
            state.a[reg] = addr;
        } else {
            for (int i = 0; i < 16; i++) {
                if (mask & (1 << i)) {
                    u32 val = (i < 8) ? state.d[i] : state.a[i - 8];
                    if (size == SIZE_LONG) {
                        write32Timed(addr, val, accessCycles);
                        accessCycles += 8;
                    } else {
                        write16Timed(addr, val, accessCycles);
                        accessCycles += 4;
                    }
                    addr += inc;
                }
            }
        }
    } else {
        // To registers
        for (int i = 0; i < 16; i++) {
            if (mask & (1 << i)) {
                u32 val;
                if (size == SIZE_LONG) {
                    val = read32Timed(addr, accessCycles);
                    accessCycles += 8;
                } else {
                    val = signExtend(read16Timed(addr, accessCycles), SIZE_WORD);
                    accessCycles += 4;
                }
                
                if (i < 8) state.d[i] = val;
                else state.a[i - 8] = val;
                addr += inc;
            }
        }
        read16Timed(addr, accessCycles);
        if (mode == 3) {
            state.a[reg] = addr;
        }
    }
    
    // Per M68000 PRM: MOVEM base is 8 (reg-to-mem) or 12 (mem-to-reg).
    // The extra 4 cycles for mem-to-reg account for a trailing read cycle.
    // EA overhead for MOVEM is address computation only (independent of
    // operand size), not the full eaCycles which includes data access.
    int count = 0;
    for (int i = 0; i < 16; i++) {
        if (mask & (1 << i)) count++;
    }
    int base = toMem ? 8 : 12;
    int eaOverhead = 0;
    switch (mode) {
        case 5: eaOverhead = 4; break;  // d(An)
        case 6: eaOverhead = 6; break;  // d(An,Xn)
        case 7:
            switch (reg) {
                case 0: eaOverhead = 4; break;  // xxx.W
                case 1: eaOverhead = 8; break;  // xxx.L
                case 2: eaOverhead = 4; break;  // d(PC)
                case 3: eaOverhead = 6; break;  // d(PC,Xn)
            }
            break;
    }
    chargeCycles(base + count * (size == SIZE_LONG ? 8 : 4) + eaOverhead);
}

void M68K::op_lea() {
    int reg = (state.ir >> 9) & 0x7;
    int mode = (state.ir >> 3) & 0x7;
    int srcReg = state.ir & 0x7;
    
    state.a[reg] = getEA(mode, srcReg, SIZE_LONG);
    // LEA has its own timing table (address computation only), which is
    // generally 2-4 cycles faster than generic "base + EA read" timing.
    switch (mode) {
        case 2:  // (An)
            chargeCycles(4);
            break;
        case 5:  // d16(An)
            chargeCycles(8);
            break;
        case 6:  // d8(An,Xn)
            chargeCycles(12);
            break;
        case 7:
            switch (srcReg) {
                case 0:  // xxx.W
                case 2:  // d16(PC)
                    chargeCycles(8);
                    break;
                case 1:  // xxx.L
                case 3:  // d8(PC,Xn)
                    chargeCycles(12);
                    break;
                default:
                    // Invalid LEA addressing modes fall back to illegal decode
                    // paths in dispatcher, but keep a safe minimum here.
                    chargeCycles(4);
                    break;
            }
            break;
        default:
            chargeCycles(4);
            break;
    }
}

void M68K::op_pea() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    u32 addr = getEA(mode, reg, SIZE_LONG);
    const int accessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_LONG);
    state.a[7] -= 4;
    write16Timed(state.a[7] + 2, lowWord(addr), accessCycles);
    write16Timed(state.a[7], highWord(addr), accessCycles + 4);
    // PRM Table 8-4: (An)=12, d(An)=16, d(An,Xi)=20, xxx.W=16, xxx.L=20, d(PC)=16, d(PC,Xi)=20
    switch (mode) {
        case 2: chargeCycles(12); break;  // (An)
        case 5: chargeCycles(16); break;  // d(An)
        case 6: chargeCycles(20); break;  // d(An,Xi)
        case 7:
            switch (reg) {
                case 0: chargeCycles(16); break;  // xxx.W
                case 1: chargeCycles(20); break;  // xxx.L
                case 2: chargeCycles(16); break;  // d(PC)
                case 3: chargeCycles(20); break;  // d(PC,Xi)
                default: chargeCycles(12); break;
            }
            break;
        default: chargeCycles(12); break;
    }
}

void M68K::op_link() {
    int reg = state.ir & 0x7;
    s16 disp = static_cast<s16>(fetchWord());
    
    push32(state.a[reg]);
    state.a[reg] = state.a[7];
    state.a[7] += disp;
    
    chargeCycles(16);
}

void M68K::op_unlk() {
    int reg = state.ir & 0x7;
    
    state.a[7] = state.a[reg];
    state.a[reg] = pop32();
    
    chargeCycles(12);
}

void M68K::op_move_usp() {
    if (!inSupervisorMode()) {
        exception(8); // Privilege violation
        return;
    }
    
    int reg = state.ir & 0x7;
    if (state.ir & 0x8) {
        // USP to An
        state.a[reg] = state.usp;
    } else {
        // An to USP
        state.usp = state.a[reg];
    }
    
    chargeCycles(4);
}

void M68K::op_exg() {
    int rx = (state.ir >> 9) & 0x7;
    int ry = state.ir & 0x7;
    int mode = (state.ir >> 3) & 0x1F;
    
    u32 temp;
    switch (mode) {
        case 0x08: // Dx <-> Dy
            temp = state.d[rx];
            state.d[rx] = state.d[ry];
            state.d[ry] = temp;
            break;
        case 0x09: // Ax <-> Ay
            temp = state.a[rx];
            state.a[rx] = state.a[ry];
            state.a[ry] = temp;
            break;
        case 0x11: // Dx <-> Ay
            temp = state.d[rx];
            state.d[rx] = state.a[ry];
            state.a[ry] = temp;
            break;
    }
    
    chargeCycles(6);
}

void M68K::op_ext() {
    int reg = state.ir & 0x7;
    int opmode = (state.ir >> 6) & 0x7;
    
    if (opmode == 2) {
        // Byte to Word
        state.d[reg] = (state.d[reg] & 0xFFFF0000) | (signExtend(state.d[reg], SIZE_BYTE) & 0xFFFF);
        setLogicFlags(state.d[reg], SIZE_WORD);
    } else if (opmode == 3) {
        // Word to Long
        state.d[reg] = signExtend(state.d[reg], SIZE_WORD);
        setLogicFlags(state.d[reg], SIZE_LONG);
    }
    
    chargeCycles(4);
}

void M68K::op_swap() {
    int reg = state.ir & 0x7;
    u32 val = state.d[reg];
    state.d[reg] = (val << 16) | (val >> 16);
    
    setLogicFlags(state.d[reg], SIZE_LONG);
    chargeCycles(4);
}
