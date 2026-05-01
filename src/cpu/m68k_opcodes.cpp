// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/m68k.h"

// Opcode group dispatchers - route to specific instruction handlers
// Implementation files: m68k_ops_*.cpp

void M68K::group0() {
    // Bit manipulation/MOVEP/Immediate
    int opmode = (state.ir >> 8) & 0xF;
    
    if ((state.ir & 0x0100) && ((state.ir & 0x38) == 0x08)) {
        op_movep();
        return;
    }
    
    if (state.ir & 0x0100) {
        // Dynamic bit operations (register specifies bit number)
        switch ((state.ir >> 6) & 0x3) {
            case 0: op_btst(); break;
            case 1: op_bchg(); break;
            case 2: op_bclr(); break;
            case 3: op_bset(); break;
        }
    } else {
        switch (opmode) {
            case 0x0: op_ori(); break;
            case 0x2: op_andi(); break;
            case 0x4: op_subi(); break;
            case 0x6: op_addi(); break;
            case 0x8: // Static bit operations
                switch ((state.ir >> 6) & 0x3) {
                    case 0: op_btst(); break;
                    case 1: op_bchg(); break;
                    case 2: op_bclr(); break;
                    case 3: op_bset(); break;
                }
                break;
            case 0xA: op_eori(); break;
            case 0xC: op_cmpi(); break;
            default: op_illegal(); break;
        }
    }
}

void M68K::group1() {
    // MOVE.B
    op_move();
}

void M68K::group2() {
    // MOVE.L / MOVEA.L
    int dstMode = (state.ir >> 6) & 0x7;
    if (dstMode == 1) {
        op_movea();
    } else {
        op_move();
    }
}

void M68K::group3() {
    // MOVE.W / MOVEA.W
    int dstMode = (state.ir >> 6) & 0x7;
    if (dstMode == 1) {
        op_movea();
    } else {
        op_move();
    }
}

void M68K::group4() {
    // Miscellaneous
    int subop = (state.ir >> 8) & 0xF;
    int mode = (state.ir >> 3) & 0x7;
    
    switch (subop) {
        case 0x0:
            if ((state.ir & 0xC0) == 0xC0) {
                op_move_from_sr();
            } else {
                op_negx();
            }
            break;
        case 0x2:
            op_clr();  // 0x42xx = CLR
            break;
        case 0x4:
            if ((state.ir & 0xC0) == 0xC0) {
                op_move_to_ccr();  // 0x44C0-0x44FF = MOVE to CCR
            } else {
                op_neg();
            }
            break;
        case 0x6:
            if ((state.ir & 0xC0) == 0xC0) {
                op_move_to_sr();  // 0x46C0-0x46FF = MOVE to SR
            } else {
                op_not();
            }
            break;
        case 0x8:
            if ((state.ir & 0xC0) == 0x00) {
                op_nbcd();
            } else if ((state.ir & 0xC0) == 0x40) {
                if (mode == 0) op_swap();
                else op_pea();
            } else if ((state.ir & 0xC0) == 0x80) {
                if (mode == 0) op_ext();
                else op_movem();
            } else {
                if (mode == 0) op_ext();
                else op_movem();
            }
            break;
        case 0xA:
            if ((state.ir & 0xC0) == 0xC0) {
                op_tas();
            } else {
                op_tst();
            }
            break;
        case 0xC:
            op_movem();
            break;
        case 0xE:
            if ((state.ir & 0x80) == 0) {
                // TRAP, LINK, UNLK, MOVE USP, misc
                switch ((state.ir >> 4) & 0x7) {
                    case 4:
                        op_trap();      // 0x4E40-0x4E4F
                        break;
                    case 5:
                        if (state.ir & 0x08)
                            op_unlk();  // 0x4E58-0x4E5F
                        else
                            op_link();  // 0x4E50-0x4E57
                        break;
                    case 6:
                        op_move_usp();
                        break;
                    case 7:
                        switch (state.ir & 0xF) {
                            case 0: op_reset(); break;
                            case 1: op_nop(); break;
                            case 2: op_stop(); break;
                            case 3: op_rte(); break;
                            case 5: op_rts(); break;
                            case 6: op_trapv(); break;
                            case 7: op_rtr(); break;
                            default: op_illegal(); break;
                        }
                        break;
                }
            } else {
                if ((state.ir & 0x40) == 0) {
                    op_jsr();
                } else {
                    op_jmp();
                }
            }
            break;
        default:
            if ((state.ir & 0x1C0) == 0x1C0) {
                op_lea();
            } else if ((state.ir & 0x1C0) == 0x180) {
                op_chk();
            } else {
                op_illegal();
            }
            break;
    }
}

void M68K::group5() {
    // ADDQ/SUBQ/Scc/DBcc
    if ((state.ir & 0xC0) == 0xC0) {
        if ((state.ir & 0x38) == 0x08) {
            op_dbcc();
        } else {
            op_scc();
        }
    } else {
        if (state.ir & 0x100) {
            op_subq();
        } else {
            op_addq();
        }
    }
}

void M68K::group6() {
    // Bcc/BSR/BRA
    int cond = (state.ir >> 8) & 0xF;
    if (cond == 0) {
        op_bra();
    } else if (cond == 1) {
        op_bsr();
    } else {
        op_bcc();
    }
}

void M68K::group7() {
    // MOVEQ
    op_moveq();
}

void M68K::group8() {
    // OR/DIV/SBCD
    if ((state.ir & 0xC0) == 0xC0) {
        if (state.ir & 0x100) {
            op_divs();
        } else {
            op_divu();
        }
    } else if ((state.ir & 0x1F0) == 0x100) {
        op_sbcd();
    } else {
        op_or();
    }
}

void M68K::group9() {
    // SUB/SUBX/SUBA
    if ((state.ir & 0xC0) == 0xC0) {
        op_suba();
    } else if ((state.ir & 0x130) == 0x100 && ((state.ir & 0xC0) != 0xC0)) {
        op_subx();
    } else {
        op_sub();
    }
}

void M68K::groupA() {
    // Line A - Unimplemented (34 cycles from exception())
    exception(10);
}

void M68K::groupB() {
    // CMP/EOR/CMPA
    if ((state.ir & 0xC0) == 0xC0) {
        op_cmpa();
    } else if ((state.ir & 0x100) && ((state.ir & 0x38) == 0x08)) {
        op_cmpm();
    } else if (state.ir & 0x100) {
        op_eor();
    } else {
        op_cmp();
    }
}

void M68K::groupC() {
    // AND/MUL/ABCD/EXG
    if ((state.ir & 0xC0) == 0xC0) {
        if (state.ir & 0x100) {
            op_muls();
        } else {
            op_mulu();
        }
    } else if ((state.ir & 0x1F0) == 0x100) {
        op_abcd();
    } else if ((state.ir & 0x1F8) == 0x140 || (state.ir & 0x1F8) == 0x148 || (state.ir & 0x1F8) == 0x188) {
        op_exg();
    } else {
        op_and();
    }
}

void M68K::groupD() {
    // ADD/ADDX/ADDA
    if ((state.ir & 0xC0) == 0xC0) {
        op_adda();
    } else if ((state.ir & 0x130) == 0x100 && ((state.ir & 0xC0) != 0xC0)) {
        op_addx();
    } else {
        op_add();
    }
}

void M68K::groupE() {
    // Shift/Rotate
    if ((state.ir & 0xC0) == 0xC0) {
        // Memory shift/rotate
        switch ((state.ir >> 9) & 0x3) {
            case 0: op_asl_asr_mem(); break;
            case 1: op_lsl_lsr_mem(); break;
            case 2: op_roxl_roxr_mem(); break;
            case 3: op_rol_ror_mem(); break;
        }
    } else {
        // Register shift/rotate
        switch ((state.ir >> 3) & 0x3) {
            case 0: op_asl_asr_reg(); break;
            case 1: op_lsl_lsr_reg(); break;
            case 2: op_roxl_roxr_reg(); break;
            case 3: op_rol_ror_reg(); break;
        }
    }
}

void M68K::groupF() {
    // Line F - Unimplemented (34 cycles from exception())
    exception(11);
}
