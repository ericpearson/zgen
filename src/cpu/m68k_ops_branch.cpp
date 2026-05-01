// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

// Branch and Control Instructions

void M68K::op_bra() {
    s8 disp8 = state.ir & 0xFF;
    s32 disp;
    
    // PC is at instruction + 2 after fetching opcode
    // Displacement is relative to THIS position (before fetching extension)
    u32 basePC = state.pc;
    
    if (disp8 == 0) {
        disp = static_cast<s16>(fetchWord());
    } else {
        disp = disp8;
    }
    
    state.pc = basePC + disp;
    chargeCycles(10);
}

void M68K::op_bsr() {
    s8 disp8 = state.ir & 0xFF;
    s32 disp;
    
    // PC is at instruction + 2 after fetching opcode
    // For word/long displacement, it's relative to THIS position (before fetching extension)
    u32 basePC = state.pc;
    
    if (disp8 == 0) {
        disp = static_cast<s16>(fetchWord());
    } else {
        disp = disp8;
    }
    
    // Push return address (current PC after fetching all extension words)
    push32(state.pc);
    
    // Branch target is always relative to instruction+2 (basePC)
    state.pc = basePC + disp;
    
    chargeCycles(18);
}

void M68K::op_bcc() {
    int cond = (state.ir >> 8) & 0xF;
    s8 disp8 = state.ir & 0xFF;
    
    // PC is now at instruction + 2 (after fetching opcode)
    // Displacement is relative to THIS position (before fetching extension)
    u32 basePC = state.pc;
    s32 disp;
    
    if (disp8 == 0) {
        // Word displacement follows
        disp = static_cast<s16>(fetchWord());
    } else {
        // Short displacement in opcode
        disp = disp8;
    }
    
    if (testCondition(cond)) {
        // Branch taken - displacement is relative to instruction+2
        state.pc = basePC + disp;
        chargeCycles(10);
    } else {
        chargeCycles((disp8 == 0) ? 12 : 8);
    }
}

void M68K::op_dbcc() {
    int cond = (state.ir >> 8) & 0xF;
    int reg = state.ir & 0x7;
    
    // Displacement is relative to PC before fetching extension word
    u32 basePC = state.pc;
    s16 disp = static_cast<s16>(fetchWord());
    
    if (!testCondition(cond)) {
        s16 count = static_cast<s16>(state.d[reg] & 0xFFFF);
        count--;
        state.d[reg] = (state.d[reg] & 0xFFFF0000) | (count & 0xFFFF);
        
        if (count != -1) {
            state.pc = basePC + disp;
            chargeCycles(10);
        } else {
            chargeCycles(14);
        }
    } else {
        chargeCycles(12);
    }
}

void M68K::op_scc() {
    int cond = (state.ir >> 8) & 0xF;
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;
    
    u8 result = testCondition(cond) ? 0xFF : 0x00;

    // PRM: Scc Dn false=4, Dn true=6, mem=8+EA
    if (mode == 0) {
        writeEA(mode, reg, SIZE_BYTE, result);
        chargeCycles(result ? 6 : 4);
    } else {
        const int readAccessCycles = state.cycles + eaReadAccessCycles(mode, reg, SIZE_BYTE);
        u32 addr = getEA(mode, reg, SIZE_BYTE);
        read8Timed(addr, readAccessCycles);
        write8Timed(addr, result, readAccessCycles + 4);
        chargeCycles(8 + eaCycles(mode, reg, SIZE_BYTE));
    }
}

void M68K::op_jmp() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    state.pc = getEA(mode, reg, SIZE_LONG);
    // JMP timing varies by addressing mode
    switch (mode) {
        case 2: chargeCycles(8); break;   // (An)
        case 5: chargeCycles(10); break;  // d(An)
        case 6: chargeCycles(14); break;  // d(An,Xi)
        case 7:
            switch (reg) {
                case 0: chargeCycles(10); break;  // xxx.W
                case 1: chargeCycles(12); break;  // xxx.L
                case 2: chargeCycles(10); break;  // d(PC)
                case 3: chargeCycles(14); break;  // d(PC,Xi)
                default: chargeCycles(8); break;
            }
            break;
        default: chargeCycles(8); break;
    }
}

void M68K::op_jsr() {
    int mode = (state.ir >> 3) & 0x7;
    int reg = state.ir & 0x7;

    u32 addr = getEA(mode, reg, SIZE_LONG);
    push32(state.pc);
    state.pc = addr;
    // JSR timing varies by addressing mode
    switch (mode) {
        case 2: chargeCycles(16); break;  // (An)
        case 5: chargeCycles(18); break;  // d(An)
        case 6: chargeCycles(22); break;  // d(An,Xi)
        case 7:
            switch (reg) {
                case 0: chargeCycles(18); break;  // xxx.W
                case 1: chargeCycles(20); break;  // xxx.L
                case 2: chargeCycles(18); break;  // d(PC)
                case 3: chargeCycles(22); break;  // d(PC,Xi)
                default: chargeCycles(16); break;
            }
            break;
        default: chargeCycles(16); break;
    }
}

void M68K::op_rts() {
    state.pc = pop32();
    chargeCycles(16);
}

void M68K::op_rte() {
    if (!inSupervisorMode()) {
        exception(8);
        return;
    }
    
    u16 newSR = pop16() & 0xA71F;  // Mask to valid 68000 bits
    state.pc = pop32();
    
    // Handle supervisor mode change before updating SR
    if (!(newSR & FLAG_S)) {
        state.ssp = state.a[7];
        state.a[7] = state.usp;
    }
    state.sr = newSR;
    
    chargeCycles(20);
}

void M68K::op_rtr() {
    u16 ccr = pop16() & 0xFF;
    state.sr = (state.sr & 0xFF00) | ccr;
    state.pc = pop32();
    
    chargeCycles(20);
}

void M68K::op_trap() {
    int vector = state.ir & 0xF;
    exception(32 + vector);
    // yacht.txt: TRAP = 34 total; exception() already adds 34
}

void M68K::op_trapv() {
    if (getFlag(FLAG_V)) {
        exception(7);
        // yacht.txt: TRAPV (trap taken) = 34 total; exception() already adds 34
    } else {
        chargeCycles(4);
    }
}

void M68K::op_nop() {
    chargeCycles(4);
}

void M68K::op_reset() {
    if (!inSupervisorMode()) {
        exception(8);
        return;
    }
    
    // Signal external reset (bus would handle this)
    chargeCycles(132);
}

void M68K::op_stop() {
    if (!inSupervisorMode()) {
        exception(8);
        return;
    }
    
    u16 imm = fetchWord();
    state.sr = imm;
    state.stopped = true;
    
    chargeCycles(4);
}

void M68K::op_illegal() {
    // Back up PC to point to the illegal instruction
    state.pc -= 2;
    // PRM: Illegal instruction exception = 34 cycles (all from exception())
    exception(4);
}
