// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "types.h"

class Bus;

// Z80 CPU Flags
enum Z80Flags {
    Z80_FLAG_C = 0x01,  // Carry
    Z80_FLAG_N = 0x02,  // Add/Subtract
    Z80_FLAG_PV = 0x04, // Parity/Overflow
    Z80_FLAG_H = 0x10,  // Half Carry
    Z80_FLAG_Z = 0x40,  // Zero
    Z80_FLAG_S = 0x80   // Sign
};

struct Z80State {
    // Main registers
    u8 a, f;            // Accumulator and flags
    u8 b, c, d, e, h, l; // General purpose
    
    // Alternate registers
    u8 a_, f_, b_, c_, d_, e_, h_, l_;
    
    // Index registers
    u16 ix, iy;
    
    // Other registers
    u16 sp;             // Stack pointer
    u16 pc;             // Program counter
    u8 i;               // Interrupt vector
    u8 r;               // Refresh counter
    
    // State
    bool iff1, iff2;    // Interrupt flip-flops
    u8 im;              // Interrupt mode (0, 1, 2)
    bool halted;
    bool irqLine;       // /INT line state (active = true, level-triggered)
    bool afterEi;       // EI delays interrupt enable by 1 instruction
    int cycles;
};

class Z80 {
public:
    Z80();
    void reset();
    void setBus(Bus* b) { bus = b; }
    
    int execute();      // Execute one instruction, return cycles
    void interrupt();   // Assert /INT line (level-triggered)
    void clearInterrupt(); // Deassert /INT line
    void nmi();         // Non-maskable interrupt
    
    // State access
    Z80State state;
    
    // Register pair accessors
    u16 getBC() const { return (state.b << 8) | state.c; }
    u16 getDE() const { return (state.d << 8) | state.e; }
    u16 getHL() const { return (state.h << 8) | state.l; }
    u16 getAF() const { return (state.a << 8) | state.f; }
    
    void setBC(u16 v) { state.b = v >> 8; state.c = v & 0xFF; }
    void setDE(u16 v) { state.d = v >> 8; state.e = v & 0xFF; }
    void setHL(u16 v) { state.h = v >> 8; state.l = v & 0xFF; }
    void setAF(u16 v) { state.a = v >> 8; state.f = v & 0xFF; }
    
private:
    Bus* bus;
    
    // Memory access
    u8 read8(u16 addr);
    void write8(u16 addr, u8 val);
    u16 read16(u16 addr);
    void write16(u16 addr, u16 val);
    
    u8 fetch8();
    u16 fetch16();
    
    // Stack operations
    void push16(u16 val);
    u16 pop16();
    
    // Flag helpers
    void setFlag(u8 flag, bool set);
    bool getFlag(u8 flag) const { return (state.f & flag) != 0; }
    void updateSZ(u8 val);   // Update Sign, Zero flags only (for arithmetic ops)
    void updateSZP(u8 val);  // Update Sign, Zero, Parity flags (for logical ops)
    u8 parity(u8 val);
    
    // ALU operations
    void add8(u8 val, bool withCarry = false);
    void sub8(u8 val, bool withCarry = false);
    void and8(u8 val);
    void or8(u8 val);
    void xor8(u8 val);
    void cp8(u8 val);
    u8 inc8(u8 val);
    u8 dec8(u8 val);
    
    // Instruction execution
    void executeMain(u8 opcode);
    void executeCB(u8 opcode);
    void executeED(u8 opcode);
    void executeDD(u8 opcode);  // IX instructions
    void executeFD(u8 opcode);  // IY instructions
    void executeDDCB(u8 opcode, s8 disp);
    void executeFDCB(u8 opcode, s8 disp);
    bool executeIndexedMemOp(u8 opcode, u16 indexReg);
    
    // Condition check
    bool checkCondition(int cc);
    
    // Register access by index
    u8 getReg8(int idx);
    void setReg8(int idx, u8 val);
    u16 getReg16(int idx);
    void setReg16(int idx, u16 val);
    u16 getReg16SP(int idx);  // BC, DE, HL, SP variant
    void setReg16SP(int idx, u16 val);
};
