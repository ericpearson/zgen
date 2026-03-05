// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "cpu/z80.h"
#include "memory/bus.h"
#include <utility>
#include <cstdio>

Z80::Z80() : bus(nullptr) {
    reset();
}

void Z80::reset() {
    state.a = state.f = 0xFF;
    state.b = state.c = state.d = state.e = state.h = state.l = 0;
    state.a_ = state.f_ = state.b_ = state.c_ = 0;
    state.d_ = state.e_ = state.h_ = state.l_ = 0;
    state.ix = state.iy = 0xFFFF;
    state.sp = 0xFFFF;
    state.pc = 0x0000;
    state.i = state.r = 0;
    state.iff1 = state.iff2 = false;
    state.im = 0;
    state.halted = false;
    state.irqLine = false;
    state.cycles = 0;
}

u8 Z80::read8(u16 addr) {
    return bus ? bus->z80Read(addr) : 0xFF;
}

void Z80::write8(u16 addr, u8 val) {
    if (bus) bus->z80Write(addr, val);
}

u16 Z80::read16(u16 addr) {
    return read8(addr) | (read8(addr + 1) << 8);
}

void Z80::write16(u16 addr, u16 val) {
    write8(addr, val & 0xFF);
    write8(addr + 1, val >> 8);
}

u8 Z80::fetch8() {
    return read8(state.pc++);
}

u16 Z80::fetch16() {
    u16 val = read16(state.pc);
    state.pc += 2;
    return val;
}

void Z80::push16(u16 val) {
    state.sp -= 2;
    write16(state.sp, val);
}

u16 Z80::pop16() {
    u16 val = read16(state.sp);
    state.sp += 2;
    return val;
}

void Z80::setFlag(u8 flag, bool set) {
    if (set) state.f |= flag;
    else state.f &= ~flag;
}

u8 Z80::parity(u8 val) {
    val ^= val >> 4;
    val ^= val >> 2;
    val ^= val >> 1;
    return (val & 1) == 0;
}

void Z80::updateSZ(u8 val) {
    setFlag(Z80_FLAG_S, val & 0x80);
    setFlag(Z80_FLAG_Z, val == 0);
}

void Z80::updateSZP(u8 val) {
    setFlag(Z80_FLAG_S, val & 0x80);
    setFlag(Z80_FLAG_Z, val == 0);
    setFlag(Z80_FLAG_PV, parity(val));
}

void Z80::add8(u8 val, bool withCarry) {
    int carry = (withCarry && getFlag(Z80_FLAG_C)) ? 1 : 0;
    int result = state.a + val + carry;
    
    setFlag(Z80_FLAG_H, ((state.a & 0x0F) + (val & 0x0F) + carry) > 0x0F);
    setFlag(Z80_FLAG_C, result > 0xFF);
    setFlag(Z80_FLAG_PV, ((state.a ^ result) & (val ^ result) & 0x80) != 0);
    setFlag(Z80_FLAG_N, false);
    
    state.a = result & 0xFF;
    updateSZ(state.a);
}

void Z80::sub8(u8 val, bool withCarry) {
    int carry = (withCarry && getFlag(Z80_FLAG_C)) ? 1 : 0;
    int result = state.a - val - carry;
    
    setFlag(Z80_FLAG_H, ((state.a & 0x0F) - (val & 0x0F) - carry) < 0);
    setFlag(Z80_FLAG_C, result < 0);
    setFlag(Z80_FLAG_PV, ((state.a ^ val) & (state.a ^ result) & 0x80) != 0);
    setFlag(Z80_FLAG_N, true);
    
    state.a = result & 0xFF;
    updateSZ(state.a);
}

void Z80::and8(u8 val) {
    state.a &= val;
    setFlag(Z80_FLAG_C, false);
    setFlag(Z80_FLAG_N, false);
    setFlag(Z80_FLAG_H, true);
    updateSZP(state.a);
}

void Z80::or8(u8 val) {
    state.a |= val;
    setFlag(Z80_FLAG_C, false);
    setFlag(Z80_FLAG_N, false);
    setFlag(Z80_FLAG_H, false);
    updateSZP(state.a);
}

void Z80::xor8(u8 val) {
    state.a ^= val;
    setFlag(Z80_FLAG_C, false);
    setFlag(Z80_FLAG_N, false);
    setFlag(Z80_FLAG_H, false);
    updateSZP(state.a);
}

void Z80::cp8(u8 val) {
    int result = state.a - val;
    
    setFlag(Z80_FLAG_H, ((state.a & 0x0F) - (val & 0x0F)) < 0);
    setFlag(Z80_FLAG_C, result < 0);
    setFlag(Z80_FLAG_PV, ((state.a ^ val) & (state.a ^ result) & 0x80) != 0);
    setFlag(Z80_FLAG_N, true);
    setFlag(Z80_FLAG_S, result & 0x80);
    setFlag(Z80_FLAG_Z, (result & 0xFF) == 0);
}

u8 Z80::inc8(u8 val) {
    u8 result = val + 1;
    setFlag(Z80_FLAG_H, (val & 0x0F) == 0x0F);
    setFlag(Z80_FLAG_PV, val == 0x7F);
    setFlag(Z80_FLAG_N, false);
    updateSZ(result);
    return result;
}

u8 Z80::dec8(u8 val) {
    u8 result = val - 1;
    setFlag(Z80_FLAG_H, (val & 0x0F) == 0x00);
    setFlag(Z80_FLAG_PV, val == 0x80);
    setFlag(Z80_FLAG_N, true);
    updateSZ(result);
    return result;
}

bool Z80::checkCondition(int cc) {
    switch (cc) {
        case 0: return !getFlag(Z80_FLAG_Z);  // NZ
        case 1: return getFlag(Z80_FLAG_Z);   // Z
        case 2: return !getFlag(Z80_FLAG_C);  // NC
        case 3: return getFlag(Z80_FLAG_C);   // C
        case 4: return !getFlag(Z80_FLAG_PV); // PO
        case 5: return getFlag(Z80_FLAG_PV);  // PE
        case 6: return !getFlag(Z80_FLAG_S);  // P
        case 7: return getFlag(Z80_FLAG_S);   // M
    }
    return false;
}

u8 Z80::getReg8(int idx) {
    switch (idx) {
        case 0: return state.b;
        case 1: return state.c;
        case 2: return state.d;
        case 3: return state.e;
        case 4: return state.h;
        case 5: return state.l;
        case 6: return read8(getHL()); // (HL)
        case 7: return state.a;
    }
    return 0;
}

void Z80::setReg8(int idx, u8 val) {
    switch (idx) {
        case 0: state.b = val; break;
        case 1: state.c = val; break;
        case 2: state.d = val; break;
        case 3: state.e = val; break;
        case 4: state.h = val; break;
        case 5: state.l = val; break;
        case 6: write8(getHL(), val); break;
        case 7: state.a = val; break;
    }
}

u16 Z80::getReg16(int idx) {
    switch (idx) {
        case 0: return getBC();
        case 1: return getDE();
        case 2: return getHL();
        case 3: return getAF();
    }
    return 0;
}

void Z80::setReg16(int idx, u16 val) {
    switch (idx) {
        case 0: setBC(val); break;
        case 1: setDE(val); break;
        case 2: setHL(val); break;
        case 3: setAF(val); break;
    }
}

u16 Z80::getReg16SP(int idx) {
    switch (idx) {
        case 0: return getBC();
        case 1: return getDE();
        case 2: return getHL();
        case 3: return state.sp;
    }
    return 0;
}

void Z80::setReg16SP(int idx, u16 val) {
    switch (idx) {
        case 0: setBC(val); break;
        case 1: setDE(val); break;
        case 2: setHL(val); break;
        case 3: state.sp = val; break;
    }
}

void Z80::interrupt() {
    // Assert /INT line (level-triggered — stays active until cleared)
    state.irqLine = true;
}

void Z80::clearInterrupt() {
    state.irqLine = false;
}

void Z80::nmi() {
    state.halted = false;
    state.iff2 = state.iff1;
    state.iff1 = false;
    
    push16(state.pc);
    state.pc = 0x0066;
    state.cycles += 11;
}

int Z80::execute() {
    state.cycles = 0;

    // Check /INT line (level-triggered) — service if iff1 is set
    if (state.irqLine && state.iff1) {
        state.halted = false;
        state.iff1 = state.iff2 = false;
        push16(state.pc);
        switch (state.im) {
            case 0:
            case 1:
                state.pc = 0x0038;
                state.cycles += 13;
                break;
            case 2:
                state.pc = read16((state.i << 8) | 0xFF);
                state.cycles += 19;
                break;
        }
        state.irqLine = false;  // Acknowledge: clear line after servicing
        return state.cycles;
    }

    if (state.halted) {
        state.cycles = 4;
        state.r = (state.r & 0x80) | ((state.r + 1) & 0x7F);
        return state.cycles;
    }

    state.r = (state.r & 0x80) | ((state.r + 1) & 0x7F);

    u8 opcode = fetch8();
    executeMain(opcode);

    return state.cycles;
}

void Z80::executeMain(u8 opcode) {
    int x = (opcode >> 6) & 3;
    int y = (opcode >> 3) & 7;
    int z = opcode & 7;
    int p = y >> 1;
    int q = y & 1;
    
    switch (x) {
        case 0:
            switch (z) {
                case 0:
                    switch (y) {
                        case 0: // NOP
                            state.cycles += 4;
                            break;
                        case 1: // EX AF, AF'
                            std::swap(state.a, state.a_);
                            std::swap(state.f, state.f_);
                            state.cycles += 4;
                            break;
                        case 2: { // DJNZ d
                            s8 disp = static_cast<s8>(fetch8());
                            state.b--;
                            if (state.b != 0) {
                                state.pc += disp;
                                state.cycles += 13;
                            } else {
                                state.cycles += 8;
                            }
                            break;
                        }
                        case 3: { // JR d
                            s8 disp = static_cast<s8>(fetch8());
                            state.pc += disp;
                            state.cycles += 12;
                            break;
                        }
                        case 4: case 5: case 6: case 7: { // JR cc, d
                            s8 disp = static_cast<s8>(fetch8());
                            if (checkCondition(y - 4)) {
                                state.pc += disp;
                                state.cycles += 12;
                            } else {
                                state.cycles += 7;
                            }
                            break;
                        }
                    }
                    break;
                case 1:
                    if (q == 0) { // LD rp, nn
                        setReg16SP(p, fetch16());
                        state.cycles += 10;
                    } else { // ADD HL, rp
                        u32 result = getHL() + getReg16SP(p);
                        setFlag(Z80_FLAG_H, ((getHL() & 0xFFF) + (getReg16SP(p) & 0xFFF)) > 0xFFF);
                        setFlag(Z80_FLAG_C, result > 0xFFFF);
                        setFlag(Z80_FLAG_N, false);
                        setHL(result & 0xFFFF);
                        state.cycles += 11;
                    }
                    break;
                case 2:
                    switch (y) {
                        case 0: write8(getBC(), state.a); break; // LD (BC), A
                        case 1: state.a = read8(getBC()); break; // LD A, (BC)
                        case 2: write8(getDE(), state.a); break; // LD (DE), A
                        case 3: state.a = read8(getDE()); break; // LD A, (DE)
                        case 4: write16(fetch16(), getHL()); break; // LD (nn), HL
                        case 5: setHL(read16(fetch16())); break; // LD HL, (nn)
                        case 6: write8(fetch16(), state.a); break; // LD (nn), A
                        case 7: state.a = read8(fetch16()); break; // LD A, (nn)
                    }
                    state.cycles += (y >= 4) ? 16 : 7;
                    break;
                case 3:
                    if (q == 0) { // INC rp
                        setReg16SP(p, getReg16SP(p) + 1);
                    } else { // DEC rp
                        setReg16SP(p, getReg16SP(p) - 1);
                    }
                    state.cycles += 6;
                    break;
                case 4: // INC r
                    setReg8(y, inc8(getReg8(y)));
                    state.cycles += (y == 6) ? 11 : 4;
                    break;
                case 5: // DEC r
                    setReg8(y, dec8(getReg8(y)));
                    state.cycles += (y == 6) ? 11 : 4;
                    break;
                case 6: // LD r, n
                    setReg8(y, fetch8());
                    state.cycles += (y == 6) ? 10 : 7;
                    break;
                case 7:
                    switch (y) {
                        case 0: { // RLCA
                            bool c = state.a & 0x80;
                            state.a = (state.a << 1) | (c ? 1 : 0);
                            setFlag(Z80_FLAG_C, c);
                            setFlag(Z80_FLAG_H, false);
                            setFlag(Z80_FLAG_N, false);
                            break;
                        }
                        case 1: { // RRCA
                            bool c = state.a & 1;
                            state.a = (state.a >> 1) | (c ? 0x80 : 0);
                            setFlag(Z80_FLAG_C, c);
                            setFlag(Z80_FLAG_H, false);
                            setFlag(Z80_FLAG_N, false);
                            break;
                        }
                        case 2: { // RLA
                            bool c = state.a & 0x80;
                            state.a = (state.a << 1) | (getFlag(Z80_FLAG_C) ? 1 : 0);
                            setFlag(Z80_FLAG_C, c);
                            setFlag(Z80_FLAG_H, false);
                            setFlag(Z80_FLAG_N, false);
                            break;
                        }
                        case 3: { // RRA
                            bool c = state.a & 1;
                            state.a = (state.a >> 1) | (getFlag(Z80_FLAG_C) ? 0x80 : 0);
                            setFlag(Z80_FLAG_C, c);
                            setFlag(Z80_FLAG_H, false);
                            setFlag(Z80_FLAG_N, false);
                            break;
                        }
                        case 4: // DAA
                            {
                                int correction = 0;
                                bool c = getFlag(Z80_FLAG_C);
                                bool h = getFlag(Z80_FLAG_H);
                                u8 oldA = state.a;
                                if (h || (state.a & 0x0F) > 9) correction |= 0x06;
                                if (c || state.a > 0x99) { correction |= 0x60; c = true; }
                                if (getFlag(Z80_FLAG_N)) state.a -= correction;
                                else state.a += correction;
                                setFlag(Z80_FLAG_C, c);
                                // H flag reflects half-carry of the correction
                                setFlag(Z80_FLAG_H, ((oldA ^ state.a) & 0x10) != 0);
                                updateSZP(state.a);
                            }
                            break;
                        case 5: // CPL
                            state.a = ~state.a;
                            setFlag(Z80_FLAG_H, true);
                            setFlag(Z80_FLAG_N, true);
                            break;
                        case 6: // SCF
                            setFlag(Z80_FLAG_C, true);
                            setFlag(Z80_FLAG_H, false);
                            setFlag(Z80_FLAG_N, false);
                            break;
                        case 7: // CCF
                            setFlag(Z80_FLAG_H, getFlag(Z80_FLAG_C));
                            setFlag(Z80_FLAG_C, !getFlag(Z80_FLAG_C));
                            setFlag(Z80_FLAG_N, false);
                            break;
                    }
                    state.cycles += 4;
                    break;
            }
            break;
            
        case 1:
            if (z == 6 && y == 6) { // HALT
                state.halted = true;
                state.cycles += 4;
            } else { // LD r, r'
                setReg8(y, getReg8(z));
                state.cycles += (y == 6 || z == 6) ? 7 : 4;
            }
            break;
            
        case 2: // ALU A, r
            switch (y) {
                case 0: add8(getReg8(z)); break;
                case 1: add8(getReg8(z), true); break;
                case 2: sub8(getReg8(z)); break;
                case 3: sub8(getReg8(z), true); break;
                case 4: and8(getReg8(z)); break;
                case 5: xor8(getReg8(z)); break;
                case 6: or8(getReg8(z)); break;
                case 7: cp8(getReg8(z)); break;
            }
            state.cycles += (z == 6) ? 7 : 4;
            break;
            
        case 3:
            switch (z) {
                case 0: // RET cc
                    if (checkCondition(y)) {
                        state.pc = pop16();
                        state.cycles += 11;
                    } else {
                        state.cycles += 5;
                    }
                    break;
                case 1:
                    if (q == 0) { // POP rp2
                        setReg16(p, pop16());
                        state.cycles += 10;
                    } else {
                        switch (p) {
                            case 0: // RET
                                state.pc = pop16();
                                state.cycles += 10;
                                break;
                            case 1: // EXX
                                std::swap(state.b, state.b_);
                                std::swap(state.c, state.c_);
                                std::swap(state.d, state.d_);
                                std::swap(state.e, state.e_);
                                std::swap(state.h, state.h_);
                                std::swap(state.l, state.l_);
                                state.cycles += 4;
                                break;
                            case 2: // JP HL
                                state.pc = getHL();
                                state.cycles += 4;
                                break;
                            case 3: // LD SP, HL
                                state.sp = getHL();
                                state.cycles += 6;
                                break;
                        }
                    }
                    break;
                case 2: { // JP cc, nn
                    u16 addr = fetch16();
                    if (checkCondition(y)) {
                        state.pc = addr;
                    }
                    state.cycles += 10;
                    break;
                }
                case 3:
                    switch (y) {
                        case 0: // JP nn
                            state.pc = fetch16();
                            state.cycles += 10;
                            break;
                        case 1: // CB prefix
                            executeCB(fetch8());
                            break;
                        case 2: // OUT (n), A — Genesis Z80 has no I/O ports, discard
                            fetch8();
                            state.cycles += 11;
                            break;
                        case 3: // IN A, (n) — Genesis Z80 has no I/O ports, return 0xFF
                            fetch8();
                            state.a = 0xFF;
                            state.cycles += 11;
                            break;
                        case 4: { // EX (SP), HL
                            u16 tmp = read16(state.sp);
                            write16(state.sp, getHL());
                            setHL(tmp);
                            state.cycles += 19;
                            break;
                        }
                        case 5: { // EX DE, HL
                            u16 tmp = getDE();
                            setDE(getHL());
                            setHL(tmp);
                            state.cycles += 4;
                            break;
                        }
                        case 6: // DI
                            state.iff1 = state.iff2 = false;
                            state.cycles += 4;
                            break;
                        case 7: // EI
                            state.iff1 = state.iff2 = true;
                            state.cycles += 4;
                            break;
                    }
                    break;
                case 4: { // CALL cc, nn
                    u16 addr = fetch16();
                    if (checkCondition(y)) {
                        push16(state.pc);
                        state.pc = addr;
                        state.cycles += 17;
                    } else {
                        state.cycles += 10;
                    }
                    break;
                }
                case 5:
                    if (q == 0) { // PUSH rp2
                        push16(getReg16(p));
                        state.cycles += 11;
                    } else {
                        switch (p) {
                            case 0: // CALL nn
                                {
                                    u16 addr = fetch16();
                                    push16(state.pc);
                                    state.pc = addr;
                                    state.cycles += 17;
                                }
                                break;
                            case 1: // DD prefix
                                executeDD(fetch8());
                                break;
                            case 2: // ED prefix
                                executeED(fetch8());
                                break;
                            case 3: // FD prefix
                                executeFD(fetch8());
                                break;
                        }
                    }
                    break;
                case 6: { // ALU A, n
                    u8 n = fetch8();
                    switch (y) {
                        case 0: add8(n); break;
                        case 1: add8(n, true); break;
                        case 2: sub8(n); break;
                        case 3: sub8(n, true); break;
                        case 4: and8(n); break;
                        case 5: xor8(n); break;
                        case 6: or8(n); break;
                        case 7: cp8(n); break;
                    }
                    state.cycles += 7;
                    break;
                }
                case 7: // RST
                    push16(state.pc);
                    state.pc = y * 8;
                    state.cycles += 11;
                    break;
            }
            break;
    }
}

void Z80::executeCB(u8 opcode) {
    int x = (opcode >> 6) & 3;
    int y = (opcode >> 3) & 7;
    int z = opcode & 7;
    
    u8 val = getReg8(z);
    
    switch (x) {
        case 0: // Rotate/shift
            switch (y) {
                case 0: { // RLC
                    bool c = val & 0x80;
                    val = (val << 1) | (c ? 1 : 0);
                    setFlag(Z80_FLAG_C, c);
                    break;
                }
                case 1: { // RRC
                    bool c = val & 1;
                    val = (val >> 1) | (c ? 0x80 : 0);
                    setFlag(Z80_FLAG_C, c);
                    break;
                }
                case 2: { // RL
                    bool c = val & 0x80;
                    val = (val << 1) | (getFlag(Z80_FLAG_C) ? 1 : 0);
                    setFlag(Z80_FLAG_C, c);
                    break;
                }
                case 3: { // RR
                    bool c = val & 1;
                    val = (val >> 1) | (getFlag(Z80_FLAG_C) ? 0x80 : 0);
                    setFlag(Z80_FLAG_C, c);
                    break;
                }
                case 4: { // SLA
                    setFlag(Z80_FLAG_C, val & 0x80);
                    val <<= 1;
                    break;
                }
                case 5: { // SRA
                    setFlag(Z80_FLAG_C, val & 1);
                    val = (val >> 1) | (val & 0x80);
                    break;
                }
                case 6: { // SLL (undocumented)
                    setFlag(Z80_FLAG_C, val & 0x80);
                    val = (val << 1) | 1;
                    break;
                }
                case 7: { // SRL
                    setFlag(Z80_FLAG_C, val & 1);
                    val >>= 1;
                    break;
                }
            }
            setFlag(Z80_FLAG_H, false);
            setFlag(Z80_FLAG_N, false);
            updateSZP(val);
            setReg8(z, val);
            break;
        case 1: // BIT
            setFlag(Z80_FLAG_Z, !(val & (1 << y)));
            setFlag(Z80_FLAG_S, (y == 7) && (val & (1 << y)));
            setFlag(Z80_FLAG_PV, !(val & (1 << y)));
            setFlag(Z80_FLAG_H, true);
            setFlag(Z80_FLAG_N, false);
            break;
        case 2: // RES
            setReg8(z, val & ~(1 << y));
            break;
        case 3: // SET
            setReg8(z, val | (1 << y));
            break;
    }
    
    state.cycles += (z == 6) ? 15 : 8;
}

void Z80::executeED(u8 opcode) {
    int x = (opcode >> 6) & 3;
    int y = (opcode >> 3) & 7;
    int z = opcode & 7;
    int p = y >> 1;
    int q = y & 1;
    
    if (x == 1) {
        switch (z) {
            case 0: { // IN r, (C)
                u8 inVal = read8(getBC());
                if (y != 6) setReg8(y, inVal);
                updateSZP(inVal);
                setFlag(Z80_FLAG_H, false);
                setFlag(Z80_FLAG_N, false);
                state.cycles += 12;
                break;
            }
            case 1: // OUT (C), r
                write8(getBC(), y != 6 ? getReg8(y) : 0);
                state.cycles += 12;
                break;
            case 2: // SBC HL, rp / ADC HL, rp
                {
                    u16 hl = getHL();
                    u16 rp = getReg16SP(p);
                    int carry = getFlag(Z80_FLAG_C) ? 1 : 0;
                    u32 result;
                    if (q == 0) { // SBC
                        result = hl - rp - carry;
                        setFlag(Z80_FLAG_N, true);
                        setFlag(Z80_FLAG_C, result > 0xFFFF);
                        setFlag(Z80_FLAG_H, (int)((hl & 0xFFF) - (rp & 0xFFF) - carry) < 0);
                        setFlag(Z80_FLAG_PV, ((hl ^ rp) & (hl ^ result) & 0x8000) != 0);
                    } else { // ADC
                        result = hl + rp + carry;
                        setFlag(Z80_FLAG_N, false);
                        setFlag(Z80_FLAG_C, result > 0xFFFF);
                        setFlag(Z80_FLAG_H, ((hl & 0xFFF) + (rp & 0xFFF) + carry) > 0xFFF);
                        setFlag(Z80_FLAG_PV, ((hl ^ result) & (rp ^ result) & 0x8000) != 0);
                    }
                    setFlag(Z80_FLAG_Z, (result & 0xFFFF) == 0);
                    setFlag(Z80_FLAG_S, result & 0x8000);
                    setHL(result & 0xFFFF);
                    state.cycles += 15;
                }
                break;
            case 3:
                if (q == 0) { // LD (nn), rp
                    write16(fetch16(), getReg16SP(p));
                } else { // LD rp, (nn)
                    setReg16SP(p, read16(fetch16()));
                }
                state.cycles += 20;
                break;
            case 4: // NEG
                {
                    u8 a = state.a;
                    state.a = 0;
                    sub8(a);
                }
                state.cycles += 8;
                break;
            case 5:
                if (q == 0) { // RETN
                    state.iff1 = state.iff2;
                    state.pc = pop16();
                } else { // RETI
                    state.iff1 = state.iff2;
                    state.pc = pop16();
                }
                state.cycles += 14;
                break;
            case 6: // IM
                switch (y & 3) {
                    case 0: case 1: state.im = 0; break;
                    case 2: state.im = 1; break;
                    case 3: state.im = 2; break;
                }
                state.cycles += 8;
                break;
            case 7:
                switch (y) {
                    case 0: // LD I, A
                        state.i = state.a;
                        state.cycles += 9;
                        break;
                    case 1: // LD R, A
                        state.r = state.a;
                        state.cycles += 9;
                        break;
                    case 2: // LD A, I
                        state.a = state.i;
                        setFlag(Z80_FLAG_PV, state.iff2);
                        updateSZ(state.a);
                        setFlag(Z80_FLAG_H, false);
                        setFlag(Z80_FLAG_N, false);
                        state.cycles += 9;
                        break;
                    case 3: // LD A, R
                        state.a = state.r;
                        setFlag(Z80_FLAG_PV, state.iff2);
                        updateSZ(state.a);
                        setFlag(Z80_FLAG_H, false);
                        setFlag(Z80_FLAG_N, false);
                        state.cycles += 9;
                        break;
                    case 4: { // RRD
                        u8 mem = read8(getHL());
                        u8 newMem = (state.a << 4) | (mem >> 4);
                        state.a = (state.a & 0xF0) | (mem & 0x0F);
                        write8(getHL(), newMem);
                        updateSZP(state.a);
                        setFlag(Z80_FLAG_H, false);
                        setFlag(Z80_FLAG_N, false);
                        state.cycles += 18;
                        break;
                    }
                    case 5: { // RLD
                        u8 mem = read8(getHL());
                        u8 newMem = (mem << 4) | (state.a & 0x0F);
                        state.a = (state.a & 0xF0) | (mem >> 4);
                        write8(getHL(), newMem);
                        updateSZP(state.a);
                        setFlag(Z80_FLAG_H, false);
                        setFlag(Z80_FLAG_N, false);
                        state.cycles += 18;
                        break;
                    }
                    case 6: // NOP
                    case 7: // NOP
                        state.cycles += 8;
                        break;
                }
                break;
        }
    } else if (x == 2 && y >= 4) {
        // Block instructions (LDI, LDIR, LDD, LDDR, CPI, CPIR, etc.)
        switch (z) {
            case 0: // LDI/LDD/LDIR/LDDR
                {
                    u8 val = read8(getHL());
                    write8(getDE(), val);
                    
                    if (y == 4 || y == 6) { // LDI, LDIR
                        setHL(getHL() + 1);
                        setDE(getDE() + 1);
                    } else { // LDD, LDDR
                        setHL(getHL() - 1);
                        setDE(getDE() - 1);
                    }
                    setBC(getBC() - 1);
                    
                    setFlag(Z80_FLAG_PV, getBC() != 0);
                    setFlag(Z80_FLAG_H, false);
                    setFlag(Z80_FLAG_N, false);
                    
                    if ((y == 6 || y == 7) && getBC() != 0) { // LDIR, LDDR
                        state.pc -= 2;
                        state.cycles += 21;
                    } else {
                        state.cycles += 16;
                    }
                }
                break;
            case 1: // CPI/CPD/CPIR/CPDR
                {
                    u8 val = read8(getHL());
                    u8 result = state.a - val;
                    
                    if (y == 4 || y == 6) setHL(getHL() + 1);
                    else setHL(getHL() - 1);
                    setBC(getBC() - 1);
                    
                    setFlag(Z80_FLAG_S, result & 0x80);
                    setFlag(Z80_FLAG_Z, result == 0);
                    setFlag(Z80_FLAG_H, (state.a & 0x0F) < (val & 0x0F));
                    setFlag(Z80_FLAG_PV, getBC() != 0);
                    setFlag(Z80_FLAG_N, true);
                    
                    if ((y == 6 || y == 7) && getBC() != 0 && result != 0) {
                        state.pc -= 2;
                        state.cycles += 21;
                    } else {
                        state.cycles += 16;
                    }
                }
                break;
            case 2: // INI/IND/INIR/INDR
                {
                    // On Genesis, I/O reads return 0xFF (no real I/O ports)
                    u8 ioVal = 0xFF;
                    write8(getHL(), ioVal);
                    state.b--;

                    if (y == 4 || y == 6) // INI, INIR
                        setHL(getHL() + 1);
                    else // IND, INDR
                        setHL(getHL() - 1);

                    setFlag(Z80_FLAG_Z, state.b == 0);
                    setFlag(Z80_FLAG_N, true);

                    if ((y == 6 || y == 7) && state.b != 0) { // INIR, INDR
                        state.pc -= 2;
                        state.cycles += 21;
                    } else {
                        state.cycles += 16;
                    }
                }
                break;
            case 3: // OUTI/OUTD/OTIR/OTDR
                {
                    // Read from (HL) and discard (Genesis has no I/O ports)
                    read8(getHL());
                    state.b--;

                    if (y == 4 || y == 6) // OUTI, OTIR
                        setHL(getHL() + 1);
                    else // OUTD, OTDR
                        setHL(getHL() - 1);

                    setFlag(Z80_FLAG_Z, state.b == 0);
                    setFlag(Z80_FLAG_N, true);

                    if ((y == 6 || y == 7) && state.b != 0) { // OTIR, OTDR
                        state.pc -= 2;
                        state.cycles += 21;
                    } else {
                        state.cycles += 16;
                    }
                }
                break;
        }
    } else {
        state.cycles += 8; // Invalid/NOP
    }
}

bool Z80::executeIndexedMemOp(u8 opcode, u16 indexReg) {
    // LD r,(IX/IY+d): 01yyy110 (except HALT encoding y=6)
    if ((opcode & 0xC7) == 0x46) {
        int y = (opcode >> 3) & 7;
        if (y != 6) {
            s8 disp = static_cast<s8>(fetch8());
            u16 addr = indexReg + disp;
            setReg8(y, read8(addr));
            state.cycles += 19;
            return true;
        }
    }

    // LD (IX/IY+d),r: 01110zzz (except HALT opcode 0x76)
    if ((opcode & 0xF8) == 0x70 && opcode != 0x76) {
        int z = opcode & 7;
        s8 disp = static_cast<s8>(fetch8());
        u16 addr = indexReg + disp;
        write8(addr, getReg8(z));
        state.cycles += 19;
        return true;
    }

    switch (opcode) {
        case 0x34: { // INC (IX/IY+d)
            s8 disp = static_cast<s8>(fetch8());
            u16 addr = indexReg + disp;
            u8 val = read8(addr);
            write8(addr, inc8(val));
            state.cycles += 23;
            return true;
        }
        case 0x35: { // DEC (IX/IY+d)
            s8 disp = static_cast<s8>(fetch8());
            u16 addr = indexReg + disp;
            u8 val = read8(addr);
            write8(addr, dec8(val));
            state.cycles += 23;
            return true;
        }
        case 0x36: { // LD (IX/IY+d),n
            s8 disp = static_cast<s8>(fetch8());
            u8 n = fetch8();
            u16 addr = indexReg + disp;
            write8(addr, n);
            state.cycles += 19;
            return true;
        }
        default:
            break;
    }

    // ALU A,(IX/IY+d): 10yyy110
    if ((opcode & 0xC7) == 0x86) {
        int y = (opcode >> 3) & 7;
        s8 disp = static_cast<s8>(fetch8());
        u8 val = read8(indexReg + disp);

        switch (y) {
            case 0: add8(val); break;
            case 1: add8(val, true); break;
            case 2: sub8(val); break;
            case 3: sub8(val, true); break;
            case 4: and8(val); break;
            case 5: xor8(val); break;
            case 6: or8(val); break;
            case 7: cp8(val); break;
        }

        state.cycles += 19;
        return true;
    }

    return false;
}

void Z80::executeDD(u8 opcode) {
    if (opcode == 0xCB) {
        s8 disp = static_cast<s8>(fetch8());
        u8 cb_opcode = fetch8();
        executeDDCB(cb_opcode, disp);
        return;
    }

    if (executeIndexedMemOp(opcode, state.ix)) {
        return;
    }

    // Fall back to HL-substituted behavior for non-displacement IX ops.
    u16 savedHL = getHL();
    setHL(state.ix);

    executeMain(opcode);

    state.ix = getHL();
    setHL(savedHL);
}

void Z80::executeFD(u8 opcode) {
    if (opcode == 0xCB) {
        s8 disp = static_cast<s8>(fetch8());
        u8 cb_opcode = fetch8();
        executeFDCB(cb_opcode, disp);
        return;
    }

    if (executeIndexedMemOp(opcode, state.iy)) {
        return;
    }

    // Fall back to HL-substituted behavior for non-displacement IY ops.
    u16 savedHL = getHL();
    setHL(state.iy);

    executeMain(opcode);

    state.iy = getHL();
    setHL(savedHL);
}

void Z80::executeDDCB(u8 opcode, s8 disp) {
    u16 addr = state.ix + disp;
    u8 val = read8(addr);
    int y = (opcode >> 3) & 7;
    int x = (opcode >> 6) & 3;

    switch (x) {
        case 0: // Rotate/shift
            switch (y) {
                case 0: { bool c = val & 0x80; val = (val << 1) | (c ? 1 : 0); setFlag(Z80_FLAG_C, c); break; } // RLC
                case 1: { bool c = val & 1; val = (val >> 1) | (c ? 0x80 : 0); setFlag(Z80_FLAG_C, c); break; } // RRC
                case 2: { bool c = val & 0x80; val = (val << 1) | (getFlag(Z80_FLAG_C) ? 1 : 0); setFlag(Z80_FLAG_C, c); break; } // RL
                case 3: { bool c = val & 1; val = (val >> 1) | (getFlag(Z80_FLAG_C) ? 0x80 : 0); setFlag(Z80_FLAG_C, c); break; } // RR
                case 4: setFlag(Z80_FLAG_C, val & 0x80); val <<= 1; break; // SLA
                case 5: setFlag(Z80_FLAG_C, val & 1); val = (val >> 1) | (val & 0x80); break; // SRA
                case 6: setFlag(Z80_FLAG_C, val & 0x80); val = (val << 1) | 1; break; // SLL
                case 7: setFlag(Z80_FLAG_C, val & 1); val >>= 1; break; // SRL
            }
            setFlag(Z80_FLAG_H, false);
            setFlag(Z80_FLAG_N, false);
            updateSZP(val);
            write8(addr, val);
            break;
        case 1: // BIT
            setFlag(Z80_FLAG_Z, !(val & (1 << y)));
            setFlag(Z80_FLAG_S, (y == 7) && (val & (1 << y)));
            setFlag(Z80_FLAG_PV, !(val & (1 << y)));
            setFlag(Z80_FLAG_H, true);
            setFlag(Z80_FLAG_N, false);
            break;
        case 2: // RES
            write8(addr, val & ~(1 << y));
            break;
        case 3: // SET
            write8(addr, val | (1 << y));
            break;
    }
    state.cycles += 23;
}

void Z80::executeFDCB(u8 opcode, s8 disp) {
    u16 addr = state.iy + disp;
    u8 val = read8(addr);
    int y = (opcode >> 3) & 7;
    int x = (opcode >> 6) & 3;

    switch (x) {
        case 0: // Rotate/shift
            switch (y) {
                case 0: { bool c = val & 0x80; val = (val << 1) | (c ? 1 : 0); setFlag(Z80_FLAG_C, c); break; } // RLC
                case 1: { bool c = val & 1; val = (val >> 1) | (c ? 0x80 : 0); setFlag(Z80_FLAG_C, c); break; } // RRC
                case 2: { bool c = val & 0x80; val = (val << 1) | (getFlag(Z80_FLAG_C) ? 1 : 0); setFlag(Z80_FLAG_C, c); break; } // RL
                case 3: { bool c = val & 1; val = (val >> 1) | (getFlag(Z80_FLAG_C) ? 0x80 : 0); setFlag(Z80_FLAG_C, c); break; } // RR
                case 4: setFlag(Z80_FLAG_C, val & 0x80); val <<= 1; break; // SLA
                case 5: setFlag(Z80_FLAG_C, val & 1); val = (val >> 1) | (val & 0x80); break; // SRA
                case 6: setFlag(Z80_FLAG_C, val & 0x80); val = (val << 1) | 1; break; // SLL
                case 7: setFlag(Z80_FLAG_C, val & 1); val >>= 1; break; // SRL
            }
            setFlag(Z80_FLAG_H, false);
            setFlag(Z80_FLAG_N, false);
            updateSZP(val);
            write8(addr, val);
            break;
        case 1: // BIT
            setFlag(Z80_FLAG_Z, !(val & (1 << y)));
            setFlag(Z80_FLAG_S, (y == 7) && (val & (1 << y)));
            setFlag(Z80_FLAG_PV, !(val & (1 << y)));
            setFlag(Z80_FLAG_H, true);
            setFlag(Z80_FLAG_N, false);
            break;
        case 2: // RES
            write8(addr, val & ~(1 << y));
            break;
        case 3: // SET
            write8(addr, val | (1 << y));
            break;
    }
    state.cycles += 23;
}
