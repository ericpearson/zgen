// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"

class M68K;
class Z80;
class VDP;
class PSG;
class YM2612;
class Cartridge;
class Controller;

class Bus {
public:
    Bus();
    ~Bus();

    void reset();

    // Memory access
    u8 read8(u32 addr);
    u8 read8(u32 addr, int partialCycles);
    u16 read16(u32 addr);
    u16 read16(u32 addr, int partialCycles);
    void write8(u32 addr, u8 val);
    void write16(u32 addr, u16 val);

    // Z80 memory access
    u8 z80Read(u16 addr);
    void z80Write(u16 addr, u8 val);

    // Z80 bank register for 68K bus access
    u32 z80Bank;

    // Component connections
    void connectCPU(M68K* cpu) { m68k = cpu; }
    void connectZ80(Z80* cpu) { z80 = cpu; }
    void connectVDP(VDP* v) { vdp = v; }
    void connectPSG(PSG* p) { psg = p; }
    void connectYM2612(YM2612* ym) { ym2612 = ym; }
    void connectCartridge(Cartridge* c) { cartridge = c; }
    void connectController(Controller* ctrl, int port) { controllers[port] = ctrl; }

    // Cartridge
    bool loadROM(const char* path);
    void setVideoStandard(VideoStandard standard) { videoStandard_ = standard; }
    VideoStandard getVideoStandard() const { return videoStandard_; }

    // Controller state
    void setButtonState(int port, int button, bool pressed);

    // 6-button controller protocol state
    void resetControllerCounters();
    void tickControllerProtocol();

    // Z80 bus control
    bool z80BusRequested;
    bool z80Reset;
    bool isM68KBusStalled() const;

    // Debug access
    u8 getZ80Ram(int addr) const { return z80Ram[addr & 0x1FFF]; }

    friend class Genesis;

private:
    M68K* m68k;
    Z80* z80;
    VDP* vdp;
    PSG* psg;
    YM2612* ym2612;
    Cartridge* cartridge;
    Controller* controllers[2];

    // Memory regions
    u8* rom;
    u32 romSize;
    u8 ram[0x10000];      // 64KB main RAM
    u8 z80Ram[0x2000];    // 8KB Z80 RAM

    // SEGA mapper bank registers (0xA130F1-0xA130FF, odd addresses)
    // Each maps a 512KB window; bank[0] is ROM area 0x000000 (fixed), 1-7 are 0x080000-0x3FFFFF
    u8 romBankRegs[8];
    bool sramMapped;       // true when SRAM is mapped into 0x200000-0x3FFFFF

    // I/O registers
    u8 ioData[3];
    u8 ioCtrl[3];
    u8 padState[2];       // bits 0-7: Up,Down,Left,Right,A,B,C,Start
    u8 padState6[2];      // bits 0-3: X,Y,Z,Mode (6-button extra)
    u8 thCounter[2];      // TH toggle counter for 6-button detection
    u8 thTimeoutLines[2]; // Approximate 1.5 ms timeout for 6-button handshake
    u8 prevTH[2];         // Previous TH state for edge detection
    VideoStandard videoStandard_;

    u8 readControllerData(int port);
};
