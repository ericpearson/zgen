// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "memory/bus.h"
#include "memory/cartridge.h"
#include "cpu/m68k.h"
#include "cpu/z80.h"
#include "video/vdp.h"
#include "audio/ym2612.h"
#include "audio/psg.h"
#include <cstdio>
#include <cstring>

namespace {
inline u16 readBE16(const u8* data, u32 offset) {
    return (static_cast<u16>(data[offset]) << 8) | data[offset + 1];
}

inline void writeBE16(u8* data, u32 offset, u16 value) {
    data[offset] = static_cast<u8>(value >> 8);
    data[offset + 1] = static_cast<u8>(value & 0xFF);
}

inline int controllerDataPortIndex(int reg) {
    if (reg >= 1 && reg <= 3) {
        return reg - 1;
    }
    return -1;
}

inline int controllerCtrlPortIndex(int reg) {
    if (reg >= 4 && reg <= 6) {
        return reg - 4;
    }
    return -1;
}

inline bool controllerTHHigh(u8 dataReg, u8 ctrlReg) {
    if ((ctrlReg & 0x40) == 0) {
        return true;
    }
    return (dataReg & 0x40) != 0;
}

}

Bus::Bus() : m68k(nullptr), z80(nullptr), vdp(nullptr), psg(nullptr),
             ym2612(nullptr), cartridge(nullptr), rom(nullptr), romSize(0),
             z80BusRequested(false), z80Reset(true), z80Bank(0),
             sramMapped(false), videoStandard_(VideoStandard::NTSC) {
    controllers[0] = nullptr;
    controllers[1] = nullptr;
    std::memset(ram, 0, sizeof(ram));
    std::memset(z80Ram, 0, sizeof(z80Ram));
    std::memset(ioData, 0x7F, sizeof(ioData));
    std::memset(ioCtrl, 0, sizeof(ioCtrl));
    padState[0] = 0xFF;
    padState[1] = 0xFF;
    std::memset(padState6, 0xFF, sizeof(padState6));
    std::memset(thCounter, 0, sizeof(thCounter));
    std::memset(thTimeoutLines, 0, sizeof(thTimeoutLines));
    std::memset(prevTH, 0x40, sizeof(prevTH));  // TH starts high
    std::memset(romBankRegs, 0, sizeof(romBankRegs));
    for (int i = 0; i < 8; i++) romBankRegs[i] = i;  // identity mapping
}

Bus::~Bus() {
    delete[] rom;
}

void Bus::reset() {
    std::memset(ram, 0, sizeof(ram));
    std::memset(z80Ram, 0, sizeof(z80Ram));
    std::memset(ioData, 0x7F, sizeof(ioData));
    std::memset(ioCtrl, 0, sizeof(ioCtrl));
    padState[0] = 0xFF;
    padState[1] = 0xFF;
    std::memset(padState6, 0xFF, sizeof(padState6));
    std::memset(thCounter, 0, sizeof(thCounter));
    std::memset(thTimeoutLines, 0, sizeof(thTimeoutLines));
    std::memset(prevTH, 0x40, sizeof(prevTH));
    std::memset(romBankRegs, 0, sizeof(romBankRegs));
    for (int i = 0; i < 8; i++) romBankRegs[i] = i;
    sramMapped = false;
    z80BusRequested = false;
    z80Reset = true;
    z80Bank = 0;
}

void Bus::resetControllerCounters() {
    thCounter[0] = 0;
    thCounter[1] = 0;
    thTimeoutLines[0] = 0;
    thTimeoutLines[1] = 0;
}

void Bus::tickControllerProtocol() {
    for (int port = 0; port < 2; ++port) {
        if (thTimeoutLines[port] == 0) {
            continue;
        }
        --thTimeoutLines[port];
        if (thTimeoutLines[port] == 0) {
            thCounter[port] = 0;
        }
    }
}

bool Bus::isM68KBusStalled() const {
    return vdp && vdp->is68kDMABusy();
}

void Bus::setButtonState(int port, int button, bool pressed) {
    if (port < 0 || port > 1) return;

    if (button >= 0 && button < 8) {
        // Standard buttons: 0=Up, 1=Down, 2=Left, 3=Right, 4=A, 5=B, 6=C, 7=Start
        if (pressed) {
            padState[port] &= ~(1u << button);
        } else {
            padState[port] |= (1u << button);
        }
    } else if (button >= 8 && button < 12) {
        // 6-button extra: 8=X, 9=Y, 10=Z, 11=Mode
        int bit = button - 8;
        if (pressed) {
            padState6[port] &= ~(1u << bit);
        } else {
            padState6[port] |= (1u << bit);
        }
    }
}

u8 Bus::readControllerData(int port) {
    if (port < 0 || port > 1) return 0x7F;

    const bool thHigh = controllerTHHigh(ioData[port], ioCtrl[port]);

    // Detect TH transitions for 6-button protocol
    const bool oldTH = (prevTH[port] & 0x40) != 0;
    if (thHigh != oldTH) {
        prevTH[port] = thHigh ? 0x40 : 0x00;
        if (!thHigh && thCounter[port] < 4) {
            // TH falling edges advance the 6-button handshake state.
            thCounter[port]++;
        }
        // Reset the handshake if TH stops toggling for about 1.5 ms.
        thTimeoutLines[port] = 24;
    }

    int phase = thCounter[port];
    if (thHigh && phase >= 4) {
        // After the 8th low read the controller returns to the idle TH-high state.
        thCounter[port] = 0;
        phase = 0;
    }

    u8 v = 0x3F;

    if (thHigh) {
        if (phase == 3) {
            // 7th read: Z, Y, X, Mode, B, C
            v = 0x40;
            v |= (padState6[port] & (1 << 2)) ? 0x01 : 0;  // Z
            v |= (padState6[port] & (1 << 1)) ? 0x02 : 0;  // Y
            v |= (padState6[port] & (1 << 0)) ? 0x04 : 0;  // X
            v |= (padState6[port] & (1 << 3)) ? 0x08 : 0;  // Mode
            v |= (padState[port] & (1 << 5)) ? 0x10 : 0;   // B
            v |= (padState[port] & (1 << 6)) ? 0x20 : 0;   // C
        } else {
            // Idle / normal TH-high read: Up, Down, Left, Right, B, C
            v = 0x40;
            v |= (padState[port] & (1 << 0)) ? 0x01 : 0;   // Up
            v |= (padState[port] & (1 << 1)) ? 0x02 : 0;   // Down
            v |= (padState[port] & (1 << 2)) ? 0x04 : 0;   // Left
            v |= (padState[port] & (1 << 3)) ? 0x08 : 0;   // Right
            v |= (padState[port] & (1 << 5)) ? 0x10 : 0;   // B
            v |= (padState[port] & (1 << 6)) ? 0x20 : 0;   // C
        }
    } else {
        // TH-low reads always expose A and Start on the upper bits.
        v = 0x00;
        v |= (padState[port] & (1 << 4)) ? 0x10 : 0;       // A
        v |= (padState[port] & (1 << 7)) ? 0x20 : 0;       // Start

        if (phase >= 4) {
            // 8th read: lower nibble is high / don't care on the official pad.
            v |= 0x0F;
        } else if (phase == 3) {
            // 6th read: lower nibble all low identifies the 6-button pad.
        } else {
            // Normal TH-low read: Up, Down, 0, 0, A, Start
            v |= (padState[port] & (1 << 0)) ? 0x01 : 0;   // Up
            v |= (padState[port] & (1 << 1)) ? 0x02 : 0;   // Down
        }
    }

    // TH reads back the currently driven level when configured as output.
    if (thHigh) {
        v |= 0x40;
    } else {
        v &= ~0x40;
    }

    return v;
}

bool Bus::loadROM(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    
    fseek(f, 0, SEEK_END);
    romSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    delete[] rom;
    rom = new u8[romSize];
    fread(rom, 1, romSize, f);
    fclose(f);
    
    return true;
}

u8 Bus::read8(u32 addr) {
    return read8(addr, m68k ? m68k->state.cycles : 0);
}

u8 Bus::read8(u32 addr, int partialCycles) {
    addr &= 0xFFFFFF;

    // ROM / SRAM: 0x000000 - 0x3FFFFF
    if (addr < 0x400000) {
        // SRAM mapped into 0x200000-0x3FFFFF when enabled
        if (sramMapped && cartridge && cartridge->hasSRAM() && addr >= 0x200000) {
            return cartridge->read8(addr);
        }
        // SEGA mapper bank switching: each 512KB window can be remapped
        u32 bank = addr >> 19;  // 0-7 (each bank = 512KB)
        u32 mappedAddr = (static_cast<u32>(romBankRegs[bank]) << 19) | (addr & 0x7FFFF);
        if (mappedAddr < romSize) return rom[mappedAddr];
        return 0xFF;
    }
    
    // RAM: 0xFF0000 - 0xFFFFFF
    if (addr >= 0xFF0000) {
        return ram[addr & 0xFFFF];
    }
    
    // Z80 area: 0xA00000 - 0xA0FFFF
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            if (!(z80BusRequested || z80Reset)) {
                return 0xFF;
            }
            return z80Ram[addr & 0x1FFF];
        }
        // YM2612 at 0xA04000-0xA04003
        // All four addresses return the same status byte on real hardware.
        if (addr >= 0xA04000 && addr < 0xA04004) {
            return ym2612 ? ym2612->readStatus() : 0;
        }
        return 0xFF;
    }
    
    // I/O area: 0xA10000 - 0xA1001F
    if (addr >= 0xA10000 && addr < 0xA10020) {
        u8 ioAddr = addr & 0x1F;
        switch (ioAddr) {
            case 0x00: // Version register (odd byte)
            case 0x01:
                // Bit 7: 0=domestic, 1=overseas
                // Bit 6: 0=NTSC, 1=PAL  
                // Bit 5: 0=no expansion, 1=expansion
                // Bits 0-3: Hardware version
                return static_cast<u8>(0x80 | (videoStandard_ == VideoStandard::PAL ? 0x40 : 0x00));
            case 0x02: // Data port 1 (odd byte = data)
            case 0x03:
                return readControllerData(0);
            case 0x04: // Data port 2
            case 0x05:
                return readControllerData(1);
            case 0x06: // Expansion port
            case 0x07:
                return ioData[2];
            case 0x08: // Ctrl port 1
            case 0x09:
                return ioCtrl[0];
            case 0x0A: // Ctrl port 2
            case 0x0B:
                return ioCtrl[1];
            case 0x0C: // Ctrl expansion
            case 0x0D:
                return ioCtrl[2];
            default:
                return 0xFF;
        }
    }
    
    // Z80 bus request status (0xA11100)
    if (addr == 0xA11100) {
        // BUSREQ status (D8): 0 = stopped/68K can access, 1 = Z80 running.
        bool z80Running = !z80BusRequested && !z80Reset;
        return z80Running ? 0x01 : 0x00;
    }
    if (addr == 0xA11101) {
        return 0x00;
    }
    
    // SEGA mapper registers (0xA130F0-0xA130FF) - read back bank values
    if (addr >= 0xA130F0 && addr <= 0xA130FF) {
        int reg = (addr & 0x0F) >> 1;
        if (addr & 1) return romBankRegs[reg];
        return sramMapped ? 1 : 0;  // Even addresses: bit 0 = SRAM mapped
    }

    // TMSS (0xA14000) - Trademark Security System
    if (addr >= 0xA14000 && addr < 0xA14004) {
        return 0x00;  // TMSS not enforced
    }

    // VDP: 0xC00000 - 0xC0001F
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return 0xFF;
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                return (addr & 1) ? (vdp->readData() & 0xFF) : (vdp->readData() >> 8);
            case 2: case 3: { // Control port (status)
                u16 s = vdp->readControl(partialCycles);
                return (addr & 1) ? (s & 0xFF) : (s >> 8);
            }
            case 4: case 5: case 6: case 7:  // HV counter
                return (addr & 1) ? (vdp->readHVCounter() & 0xFF) : (vdp->readHVCounter() >> 8);
            default:
                return 0xFF;
        }
    }
    
    return 0xFF;
}

u16 Bus::read16(u32 addr) {
    return read16(addr, m68k ? m68k->state.cycles : 0);
}

u16 Bus::read16(u32 addr, int partialCycles) {
    addr &= 0xFFFFFF;

    // VDP needs special handling - single 16-bit read
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return 0xFFFF;
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                return vdp->readData();
            case 2: case 3:  // Control port (status)
                // Pass partial instruction cycles for cycle-accurate HBlank
                // status. On real hardware the VDP runs concurrently so the
                // status reflects the VDP position at the bus access, not the
                // start of the instruction.
                return vdp->readControl(partialCycles);
            case 4: case 5: case 6: case 7:  // HV counter
                return vdp->readHVCounter();
            default:
                return 0xFFFF;
        }
    }

    // ROM / SRAM
    if (addr < 0x400000) {
        if (sramMapped && cartridge && cartridge->hasSRAM() && addr >= 0x200000) {
            return cartridge->read16(addr);
        }

        const u32 bank = addr >> 19;
        const u32 bankOffset = addr & 0x7FFFF;
        const u32 mappedAddr = (static_cast<u32>(romBankRegs[bank]) << 19) | bankOffset;
        if (bankOffset < 0x7FFFF && mappedAddr + 1 < romSize) {
            return readBE16(rom, mappedAddr);
        }
        return (read8(addr) << 8) | read8(addr + 1);
    }

    // RAM
    if (addr >= 0xFF0000) {
        const u32 ramOffset = addr & 0xFFFF;
        return (static_cast<u16>(ram[ramOffset]) << 8) | ram[(ramOffset + 1) & 0xFFFF];
    }

    // Z80 area
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            if (!(z80BusRequested || z80Reset)) {
                return 0xFFFF;
            }
            const u16 z80Addr = addr & 0x1FFF;
            return (static_cast<u16>(z80Ram[z80Addr]) << 8) | z80Ram[(z80Addr + 1) & 0x1FFF];
        }
        if (addr >= 0xA04000 && addr < 0xA04004) {
            return (static_cast<u16>(read8(addr)) << 8) | read8(addr + 1);
        }
    }

    return (read8(addr) << 8) | read8(addr + 1);
}

void Bus::write8(u32 addr, u8 val) {
    addr &= 0xFFFFFF;

    // SRAM writes (0x200000-0x3FFFFF when mapped)
    if (addr >= 0x200000 && addr < 0x400000) {
        if (sramMapped && cartridge && cartridge->hasSRAM()) {
            cartridge->write8(addr, val);
        }
        return;
    }

    // RAM
    if (addr >= 0xFF0000) {
        ram[addr & 0xFFFF] = val;
        return;
    }
    
    // Z80 area
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            if (z80BusRequested || z80Reset) {
                z80Ram[addr & 0x1FFF] = val;
            }
            return;
        }
        // YM2612 at 0xA04000-0xA04003
        if (addr >= 0xA04000 && addr < 0xA04004) {
            if (ym2612) {
                int port = (addr >> 1) & 1;
                if (addr & 1) {
                    ym2612->writeData(val, port);
                } else {
                    ym2612->writeAddress(val, port);
                }
            }
            return;
        }
        return;
    }
    
    // I/O area
    if (addr >= 0xA10000 && addr < 0xA10020) {
        int reg = (addr & 0x1F) >> 1;
        int dataIndex = controllerDataPortIndex(reg);
        int ctrlIndex = controllerCtrlPortIndex(reg);
        if (dataIndex >= 0) {
            ioData[dataIndex] = val;
        } else if (ctrlIndex >= 0) {
            ioCtrl[ctrlIndex] = val;
        }
        return;
    }
    
    // Z80 bus control - only even addresses matter (high byte of word write)
    if (addr == 0xA11100) {
        z80BusRequested = (val & 1) != 0;
        return;
    }
    if (addr == 0xA11101) {
        return;  // Ignore odd address writes
    }
    
    if (addr == 0xA11200) {
        bool newReset = !(val & 1);
        if (newReset && !z80Reset && z80) {
            z80->reset();
        }
        z80Reset = newReset;
        return;
    }
    if (addr == 0xA11201) {
        return;  // Ignore odd address writes
    }
    
    // SEGA mapper (0xA130F0-0xA130FF)
    if (addr >= 0xA130F0 && addr <= 0xA130FF) {
        if (addr == 0xA130F1) {
            // Bank 0 control: bit 0 = SRAM mapping enable
            sramMapped = (val & 1) != 0;
        } else if (addr & 1) {
            // Odd addresses: bank register for 512KB windows
            int reg = (addr & 0x0F) >> 1;
            if (reg > 0 && reg < 8) {
                romBankRegs[reg] = val & 0x3F;
            }
        }
        return;
    }

    // TMSS (0xA14000) - just accept writes
    if (addr >= 0xA14000 && addr < 0xA14004) {
        return;  // Ignore TMSS writes
    }
    
    // PSG at 0xC00011
    if (addr == 0xC00011) {
        if (psg) psg->write(val);
        return;
    }

    // VDP
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return;
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                vdp->writeDataByte(val, (addr & 1) != 0);
                break;
            case 2: case 3:  // Control port
                vdp->writeControl((val << 8) | val);
                break;
            default:
                break;
        }
        return;
    }
}

void Bus::write16(u32 addr, u16 val) {
    addr &= 0xFFFFFF;

    // VDP word writes need special handling
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return;
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                vdp->writeData(val);
                break;
            case 2: case 3:  // Control port
                vdp->writeControl(val);
                break;
            default:
                break;
        }
        return;
    }

    // SRAM writes
    if (addr >= 0x200000 && addr < 0x400000) {
        if (sramMapped && cartridge && cartridge->hasSRAM()) {
            cartridge->write16(addr, val);
        }
        return;
    }

    // RAM
    if (addr >= 0xFF0000) {
        writeBE16(ram, addr & 0xFFFF, val);
        return;
    }

    // Z80 area
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            if (z80BusRequested || z80Reset) {
                const u16 z80Addr = addr & 0x1FFF;
                z80Ram[z80Addr] = static_cast<u8>(val >> 8);
                z80Ram[(z80Addr + 1) & 0x1FFF] = static_cast<u8>(val & 0xFF);
            }
            return;
        }
    }

    // Other addresses use byte writes
    write8(addr, val >> 8);
    write8(addr + 1, val & 0xFF);
}

u8 Bus::z80Read(u16 addr) {
    // Z80 RAM: $0000-$1FFF
    if (addr < 0x2000) {
        return z80Ram[addr];
    }
    
    // Z80 RAM mirror: $2000-$3FFF
    if (addr < 0x4000) {
        return z80Ram[addr & 0x1FFF];
    }
    
    // YM2612: $4000-$5FFF
    // All four addresses ($4000-$4003) return the status register on read.
    // The YM2612 has a single status byte; reads from any mirrored address
    // within this range return the same value.  GEMS (and other Z80 sound
    // drivers) read the busy flag from $4002, so this must not return $FF.
    if (addr < 0x6000) {
        return ym2612 ? ym2612->readStatus() : 0;
    }
    
    // Bank register: $6000-$60FF
    if (addr >= 0x6000 && addr < 0x6100) {
        return 0; // Write-only
    }
    
    // PSG: $7F00-$7FFF (directly addressable only from 68K)
    
    // 68K bus access: $8000-$FFFF (32KB window into 68K address space)
    if (addr >= 0x8000) {
        u32 addr68k = (z80Bank << 15) | (addr & 0x7FFF);
        return read8(addr68k);
    }
    
    return 0xFF;
}

void Bus::z80Write(u16 addr, u8 val) {
    // Z80 RAM: $0000-$1FFF
    if (addr < 0x2000) {
        z80Ram[addr] = val;
        return;
    }
    
    // Z80 RAM mirror: $2000-$3FFF
    if (addr < 0x4000) {
        z80Ram[addr & 0x1FFF] = val;
        return;
    }
    
    // YM2612: $4000-$5FFF
    if (addr < 0x6000) {
        if (!ym2612) return;
        int port = (addr >> 1) & 1; // Port 0 or 1
        if (addr & 1) {
            ym2612->writeData(val, port);
        } else {
            ym2612->writeAddress(val, port);
        }
        return;
    }
    
    // Bank register: $6000-$60FF
    if (addr >= 0x6000 && addr < 0x6100) {
        // 9-bit shift register - each write shifts right, new bit enters at MSB (bit 8)
        z80Bank = ((z80Bank >> 1) | ((val & 1) << 8)) & 0x1FF;
        return;
    }
    
    // PSG: $7F00-$7FFF
    if (addr >= 0x7F00) {
        if (psg) psg->write(val);
        return;
    }
    
    // 68K bus access: $8000-$FFFF
    if (addr >= 0x8000) {
        u32 addr68k = (z80Bank << 15) | (addr & 0x7FFF);
        write8(addr68k, val);
    }
}
