// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include <functional>

class Bus;

enum StatusFlags {
    FLAG_C = 0x0001,    // Carry
    FLAG_V = 0x0002,    // Overflow
    FLAG_Z = 0x0004,    // Zero
    FLAG_N = 0x0008,    // Negative
    FLAG_X = 0x0010,    // Extend
    FLAG_I0 = 0x0100,   // Interrupt mask bit 0
    FLAG_I1 = 0x0200,   // Interrupt mask bit 1
    FLAG_I2 = 0x0400,   // Interrupt mask bit 2
    FLAG_S = 0x2000,    // Supervisor mode
    FLAG_T = 0x8000,    // Trace mode
};

enum Size {
    SIZE_BYTE = 0,
    SIZE_WORD = 1,
    SIZE_LONG = 2
};

struct M68KState {
    u32 d[8];           // Data registers D0-D7
    u32 a[8];           // Address registers A0-A7 (A7 = USP or SSP)
    u32 pc;             // Program counter
    u32 usp;            // User stack pointer
    u32 ssp;            // Supervisor stack pointer
    u16 sr;             // Status register

    u16 ir;             // Instruction register
    int cycles;         // Cycles for current instruction
    bool stopped;       // STOP instruction state
    bool halted;        // HALT state
    int pendingInterrupt; // Pending interrupt level (-1 = none)
};

class M68K {
public:
    M68K();
    void connectBus(Bus* bus);
    void reset();
    int execute();                    // Execute one instruction, return cycles
    void interrupt(int level);        // Request interrupt
    void invalidatePrefetch();
    void setInterruptAckCallback(std::function<void(int)> cb) { interruptAckCallback_ = cb; }
    void setMemoryTimingOffset(int cycles) { memoryTimingOffset_ = cycles; }
    
    // Accessors
    u32 getPC() const { return state.pc; }
    u16 getSR() const { return state.sr; }
    u32 getLastPC() const { return lastPC_; }
    u16 getLastIR() const { return lastIR_; }
    const M68KState& getState() const { return state; }
    int getMemoryTimingOffset() const { return memoryTimingOffset_; }

    M68KState state;
    u64 totalCycles_ = 0;  // Cumulative 68K cycles for E-clock phase calculation
    int traceFrameNumber_ = 0;  // Set by genesis.cpp for frame-gated tracing
    int traceScanline_ = 0;     // Set by genesis.cpp for scanline context
    u64 masterCycles_ = 0; // Cumulative master clocks (M68K cycles * 7 + VDP time)

    // 68K bus refresh: DRAM refresh steals 2 M68K cycles every 128 M68K cycles.
    // Counter-based accumulation applied at sync points (VDP/IO access,
    // scanline boundaries, pre-interrupt), matching BlastEm's model.
    static constexpr u64 kRefreshInterval = 128 * 7;  // 896 master clocks
    static constexpr int kRefreshDelay = 2;            // M68K cycles per penalty
    u32 refreshCounter_ = 0;       // accumulated master clocks mod interval
    u64 refreshLastSync_ = 0;      // masterCycles_ at last refresh sync
    bool refreshEnabled_ = false;

    // Sync-point refresh: accumulate elapsed time and apply penalties.
    // Returns the number of M68K penalty cycles applied.
    int applyRefresh();

    // Free-access refresh: apply penalties for time BEFORE the current
    // 4-MCLK bus access, but the access itself is penalty-free.
    // Called before VDP/IO port reads/writes.
    int applyRefreshFreeAccess();

    // No-wait refresh: advance counter without penalties.
    // Called during FIFO stalls, DMA stalls, and Z80 bus-busy periods.
    void advanceRefreshNoWait(int m68kCycles);

    // Memory interface
    u8 read8(u32 addr);
    u16 read16(u32 addr);
    u32 read32(u32 addr);
    u8 read8Timed(u32 addr, int partialCycles);
    u16 read16Timed(u32 addr, int partialCycles);
    u32 read32Timed(u32 addr, int partialCycles);
    void write8Timed(u32 addr, u8 val, int partialCycles);
    void write16Timed(u32 addr, u16 val, int partialCycles);
    void write32Timed(u32 addr, u32 val, int partialCycles);
    void write8(u32 addr, u8 val);
    void write16(u32 addr, u16 val);
    void write32(u32 addr, u32 val);

    // For debugging
    u16 peekWord(u32 addr);

private:
    Bus* bus;

    // Fetch
    void refillPrefetch();
    u16 fetchWord();
    u32 fetchLong();

    // Stack operations
    void push16(u16 val);
    void push32(u32 val);
    u16 pop16();
    u32 pop32();

    // Addressing modes
    u32 getEA(int mode, int reg, int size);
    u32 readEA(int mode, int reg, int size);
    void writeEA(int mode, int reg, int size, u32 val);
    u32 readMemSized(u32 addr, int size);
    void writeMemSized(u32 addr, int size, u32 val);
    void writeMemSizedTimed(u32 addr, int size, u32 val, int partialCycles);
    int eaReadAccessCycles(int mode, int reg, int size);

    // Flag helpers
    void setFlag(u16 flag, bool set);
    bool getFlag(u16 flag);
    int getIntMask();
    void setIntMask(int level);
    bool inSupervisorMode();
    void setSupervisorMode(bool super);

    // Condition testing
    bool testCondition(int cc);

    // Exception handling
    void exception(int vector);
    void processInterrupt();

    std::function<void(int)> interruptAckCallback_;

    // 68000 prefetch queue approximation (2 words).
    bool prefetchValid_;
    u16 prefetchWord0_;
    u16 prefetchWord1_;
    u32 prefetchPC_;
    u32 prefetchNextPC_;
    u32 lastPC_;
    u16 lastIR_;
    int memoryTimingOffset_ = 0;

    // Instruction handlers
    void executeInstruction();

    // Opcode groups
    void group0();   // Bit manipulation/MOVEP/Immediate
    void group1();   // MOVE.B
    void group2();   // MOVE.L
    void group3();   // MOVE.W
    void group4();   // Miscellaneous
    void group5();   // ADDQ/SUBQ/Scc/DBcc
    void group6();   // Bcc/BSR/BRA
    void group7();   // MOVEQ
    void group8();   // OR/DIV/SBCD
    void group9();   // SUB/SUBX
    void groupA();   // Line A (unimplemented)
    void groupB();   // CMP/EOR
    void groupC();   // AND/MUL/ABCD/EXG
    void groupD();   // ADD/ADDX
    void groupE();   // Shift/Rotate
    void groupF();   // Line F (unimplemented)

    // Individual instruction implementations
    void op_ori();
    void op_andi();
    void op_subi();
    void op_addi();
    void op_eori();
    void op_cmpi();
    void op_btst();
    void op_bchg();
    void op_bclr();
    void op_bset();
    void op_movep();
    void op_move();
    void op_move_from_sr();
    void op_move_to_sr();
    void op_move_to_ccr();
    void op_movea();
    void op_negx();
    void op_clr();
    void op_neg();
    void op_not();
    void op_ext();
    void op_nbcd();
    void op_swap();
    void op_pea();
    void op_illegal();
    void op_tas();
    void op_tst();
    void op_trap();
    void op_link();
    void op_unlk();
    void op_move_usp();
    void op_reset();
    void op_nop();
    void op_stop();
    void op_rte();
    void op_rts();
    void op_trapv();
    void op_rtr();
    void op_jsr();
    void op_jmp();
    void op_movem();
    void op_lea();
    void op_chk();
    void op_addq();
    void op_subq();
    void op_scc();
    void op_dbcc();
    void op_bra();
    void op_bsr();
    void op_bcc();
    void op_moveq();
    void op_or();
    void op_divu();
    void op_divs();
    void op_sbcd();
    void op_sub();
    void op_subx();
    void op_suba();
    void op_cmp();
    void op_cmpa();
    void op_cmpm();
    void op_eor();
    void op_and();
    void op_mulu();
    void op_muls();
    void op_abcd();
    void op_exg();
    void op_add();
    void op_addx();
    void op_adda();
    void op_asl_asr_mem();
    void op_lsl_lsr_mem();
    void op_roxl_roxr_mem();
    void op_rol_ror_mem();
    void op_asl_asr_reg();
    void op_lsl_lsr_reg();
    void op_roxl_roxr_reg();
    void op_rol_ror_reg();

    // EA cycle timing helpers
    int eaCycles(int mode, int reg, int size);
    int eaWriteCycles(int mode, int reg, int size);

    // ALU helpers
    u32 doAdd(u32 src, u32 dst, int size, bool withX = false);
    u32 doSub(u32 src, u32 dst, int size, bool withX = false);
    void setLogicFlags(u32 result, int size);
    u32 signExtend(u32 val, int fromSize);
    u32 maskResult(u32 val, int size);
    bool isNegative(u32 val, int size);
};
