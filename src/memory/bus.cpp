// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "memory/bus.h"
#include "memory/cartridge.h"
#include "genesis.h"
#include "cpu/m68k.h"
#include "cpu/z80.h"
#include "video/vdp.h"
#include "audio/ym2612.h"
#include "audio/psg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr u64 Z80_BUSACK_ASSERT_MCLKS = 3 * 15;
constexpr u64 Z80_BUSACK_RELEASE_MCLKS = 1 * 15;

inline bool fifoTimingLogEnabled() {
    static int e = -1;
    if (e < 0) { const char* v = std::getenv("GENESIS_LOG_FIFO_TIMING"); e = (v && std::atoi(v)) ? 1 : 0; }
    return e != 0;
}

inline bool vdpPortWaitDiagEnabled() {
    static int e = -1;
    if (e < 0) {
        const char* v = std::getenv("GENESIS_VDP_PORT_WAIT_DIAG");
        e = (v && v[0] != '0') ? 1 : 0;
    }
    return e != 0;
}
} // namespace

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

bool ramWriteWatchEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = std::getenv("GENESIS_LOG_RAM_WRITES");
        enabled = (env && env[0]) ? 1 : 0;
    }
    return enabled != 0;
}

bool ramWriteWatchMatches(u32 addr) {
    if (!ramWriteWatchEnabled()) {
        return false;
    }

    static bool parsed = false;
    static constexpr int kMaxWatchRanges = 16;
    struct WatchRange {
        u32 start;
        u32 end;
    };
    static WatchRange ranges[kMaxWatchRanges];
    static int rangeCount = 0;
    if (!parsed) {
        parsed = true;
        const char* env = std::getenv("GENESIS_LOG_RAM_WRITES");
        if (!env || !env[0]) {
            return false;
        }

        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s", env);

        char* savePtr = nullptr;
        for (char* token = strtok_r(buffer, ",", &savePtr);
             token && rangeCount < kMaxWatchRanges;
             token = strtok_r(nullptr, ",", &savePtr)) {
            while (*token == ' ' || *token == '\t') {
                token++;
            }
            if (!*token) {
                continue;
            }

            char* parseEnd = nullptr;
            u32 start = static_cast<u32>(std::strtoul(token, &parseEnd, 16));
            u32 end = start;
            if (parseEnd && *parseEnd == '-') {
                end = static_cast<u32>(std::strtoul(parseEnd + 1, nullptr, 16));
            }
            start &= 0xFFFFFF;
            end &= 0xFFFFFF;
            if (end < start) {
                u32 tmp = start;
                start = end;
                end = tmp;
            }
            ranges[rangeCount++] = {start, end};
        }
    }

    addr &= 0xFFFFFF;
    for (int i = 0; i < rangeCount; i++) {
        if (addr >= ranges[i].start && addr <= ranges[i].end) {
            return true;
        }
    }
    return false;
}

}

Bus::Bus() : genesis(nullptr), m68k(nullptr), z80(nullptr), vdp(nullptr), psg(nullptr),
             ym2612(nullptr), cartridge(nullptr), rom(nullptr), romSize(0),
             z80BusRequested(false), z80BusAck(false), z80Reset(true),
             busreqAssertCycle(0), busreqDeassertCycle(0),
             busreqAssertMclk(0), busreqDeassertMclk(0), masterCycle(0),
             z80CycleCounter(0), z80Bank(0),
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

u32 Bus::getM68KA6() const {
    return m68k ? m68k->state.a[6] : 0;
}

u32 Bus::getM68KPC() const {
    return m68k ? m68k->getLastPC() : 0;
}

void Bus::addM68KZ80AreaWaitCycles(int cycles) {
    if (cycles <= 0) return;
    m68kZ80AreaWaitCycles += cycles;
    if (m68k) {
        m68k->addBusWaitCycles(cycles);
    }
}

void Bus::waitForVDPFIFOForWrite() {
    if (!vdp || !m68k) {
        return;
    }

    int guard = 0;
    while (vdp->isVDPFIFOFull() && guard++ < 4096) {
        int wait = vdp->fifoWaitCycles();
        if (wait <= 0) {
            wait = 1;
        }
        const int before = vdp->getLineCycles();
        vdp->clockM68K(wait);
        m68k->addBusWaitCycles(wait);
        if (!vdp->isVDPFIFOFull()) {
            break;
        }
        if (vdp->getLineCycles() == before &&
            vdp->advanceFifoForCpuBoundaryWait()) {
            continue;
        }
        if (vdp->getLineCycles() == before) {
            break;
        }
    }
}

void Bus::waitForVDP68KDMA() {
    if (!vdp || !m68k) {
        return;
    }

    int guard = 0;
    while (vdp->is68kDMABusy() && guard++ < 65536) {
        int wait = vdp->dmaWaitCycles();
        if (wait <= 0) {
            wait = 1;
        }

        const int lineCyclesBefore = vdp->getLineCycles();
        vdp->clockM68K(wait);
        if (vdp->getLineCycles() == lineCyclesBefore && vdp->is68kDMABusy()) {
            break;
        }
        m68k->addBusWaitCycles(wait);
    }
}

void Bus::addZ80BankAccessPenalty() {
    z80BankAccessCount++;
    z80BankAccessCycles += 3;
    pendingM68KZ80BusStallCycles += 8;
    if (genesis) {
        genesis->traceZ80BankAccessPenalty(3, 8);
    }
    if (z80) {
        z80->state.cycles += 3;
    }
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
    z80BusAck = false;
    z80Reset = true;
    busreqAssertCycle = 0;
    busreqDeassertCycle = 0;
    busreqAssertMclk = 0;
    busreqDeassertMclk = 0;
    masterCycle = 0;
    z80CycleCounter = 0;
    z80Bank = 0;
    z80BankAccessCount = 0;
    z80BankAccessCycles = 0;
    pendingM68KZ80BusStallCycles = 0;
    m68kZ80AreaWaitCycles = 0;
}

u64 Bus::m68kAccessMclk(int partialCycles) const {
    if (!m68k) {
        return 0;
    }
    if (partialCycles < 0) {
        partialCycles = 0;
    }
    return (m68k->getTotalCycles() + static_cast<u64>(partialCycles)) *
           M68K::kMclksPerM68KCycle;
}

void Bus::settleBusAckForM68KAccess(int partialCycles) {
    if (!m68k) {
        return;
    }
    const u64 accessMclk = m68kAccessMclk(partialCycles);
    if (z80BusRequested && !z80BusAck) {
        if (accessMclk >= busreqAssertMclk &&
            accessMclk - busreqAssertMclk >= Z80_BUSACK_ASSERT_MCLKS) {
            z80BusAck = true;
        }
    } else if (!z80BusRequested && z80BusAck) {
        if (accessMclk >= busreqDeassertMclk &&
            accessMclk - busreqDeassertMclk >= Z80_BUSACK_RELEASE_MCLKS) {
            z80BusAck = false;
        }
    }
}

// Update BUSACK state based on elapsed Z80 cycles.
// BUSACK takes ~3 Z80 cycles to assert and 1 cycle to release.
void Bus::updateBusAck(int z80Cycles) {
    z80CycleCounter += z80Cycles;

    if (z80BusRequested && !z80BusAck) {
        // Waiting for BUSACK to assert (~3 Z80 cycles)
        int cyclesSinceRequest = z80CycleCounter - busreqAssertCycle;
        if (cyclesSinceRequest >= 3) {
            z80BusAck = true;  // 68K now owns bus
        }
    } else if (!z80BusRequested && z80BusAck) {
        // Waiting for Z80 to resume (~1 Z80 cycle)
        int cyclesSinceDeassert = z80CycleCounter - busreqDeassertCycle;
        if (cyclesSinceDeassert >= 1) {
            z80BusAck = false;  // Z80 now owns bus
        }
    }
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

int Bus::consumeM68KZ80BusStallCycles(int maxCycles) {
    if (maxCycles <= 0 || pendingM68KZ80BusStallCycles <= 0) {
        return 0;
    }
    int consumed = pendingM68KZ80BusStallCycles;
    if (consumed > maxCycles) {
        consumed = maxCycles;
    }
    pendingM68KZ80BusStallCycles -= consumed;
    return consumed;
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
    const int cpuPartialCycles = partialCycles;
    partialCycles += m68k ? m68k->getMemoryTimingOffset() : 0;
    addr &= 0xFFFFFF;

    // ROM / SRAM: 0x000000 - 0x3FFFFF
    if (addr < 0x400000) {
        // SRAM can either be selected through the Sega mapper or directly
        // mapped after ROM for smaller carts.
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) ||
             (sramMapped && addr >= 0x200000))) {
            return cartridge->readSRAM8(addr);
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
    // 68K Z80-area IO incurs one M68K wait cycle per transaction.
    if (addr >= 0xA00000 && addr < 0xA10000) {
        addM68KZ80AreaWaitCycles();
        if (addr < 0xA02000) {
            if (!(z80BusRequested || z80Reset)) {
                return 0xFF;
            }
            return z80Ram[addr & 0x1FFF];
        }
        // YM2612 at 0xA04000-0xA04003. Port 0 returns live status; the
        // other ports return the chip's latched last status value.
        if (addr >= 0xA04000 && addr < 0xA04004) {
            return ym2612 ? ym2612->readStatus(static_cast<int>(addr & 3)) : 0;
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
        settleBusAckForM68KAccess(cpuPartialCycles);
        // Bit 0 = 0 when bus granted (68K can access), 1 when Z80 running
        // Reset asserted is not a BUSACK grant; games poll for bit 0 set
        // after halting the Z80 with reset.
        bool busGranted = z80BusAck && !z80Reset;
        return busGranted ? 0x00 : 0x01;
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
        if (m68k) m68k->applyRefreshFreeAccess();
        vdp->syncToCpuTimingOffset(partialCycles);
        if (m68k && vdpPortWaitDiagEnabled()) {
            m68k->addBusWaitCycles(1);
        }
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1: {  // Data port
                u16 data = vdp->readData();
                return (addr & 1) ? (data & 0xFF) : (data >> 8);
            }
            case 2: case 3: { // Control port (status)
                u16 s = vdp->readControl();
                return (addr & 1) ? (s & 0xFF) : (s >> 8);
            }
            case 4: case 5: case 6: case 7: { // HV counter
                u16 hv = vdp->readHVCounter();
                return (addr & 1) ? (hv & 0xFF) : (hv >> 8);
            }
            default:
                return 0xFF;
        }
    }
    
    return 0xFF;
}

u8 Bus::debugPeek8(u32 addr) const {
    addr &= 0xFFFFFF;

    if (addr < 0x400000) {
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) ||
             (sramMapped && addr >= 0x200000))) {
            return cartridge->readSRAM8(addr);
        }
        const u32 bank = addr >> 19;
        const u32 mappedAddr = (static_cast<u32>(romBankRegs[bank]) << 19) | (addr & 0x7FFFF);
        return mappedAddr < romSize ? rom[mappedAddr] : 0xFF;
    }

    if (addr >= 0xFF0000) {
        return ram[addr & 0xFFFF];
    }

    if (addr >= 0xA00000 && addr < 0xA02000) {
        return z80Ram[addr & 0x1FFF];
    }

    return 0xFF;
}

u16 Bus::read16(u32 addr) {
    return read16(addr, m68k ? m68k->state.cycles : 0);
}

u16 Bus::debugPeek16(u32 addr) const {
    addr &= 0xFFFFFF;

    if (addr < 0x400000) {
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) ||
             (sramMapped && addr >= 0x200000))) {
            return cartridge->readSRAM16(addr);
        }

        const u32 bank = addr >> 19;
        const u32 bankOffset = addr & 0x7FFFF;
        const u32 mappedAddr = (static_cast<u32>(romBankRegs[bank]) << 19) | bankOffset;
        if (bankOffset < 0x7FFFF && mappedAddr + 1 < romSize) {
            return readBE16(rom, mappedAddr);
        }
        return (static_cast<u16>(debugPeek8(addr)) << 8) | debugPeek8(addr + 1);
    }

    if (addr >= 0xFF0000) {
        const u32 ramOffset = addr & 0xFFFF;
        return (static_cast<u16>(ram[ramOffset]) << 8) | ram[(ramOffset + 1) & 0xFFFF];
    }

    if (addr >= 0xA00000 && addr < 0xA02000) {
        const u16 z80Addr = addr & 0x1FFF;
        return (static_cast<u16>(z80Ram[z80Addr]) << 8) | z80Ram[(z80Addr + 1) & 0x1FFF];
    }

    return (static_cast<u16>(debugPeek8(addr)) << 8) | debugPeek8(addr + 1);
}

u32 Bus::debugPeek32(u32 addr) const {
    return (static_cast<u32>(debugPeek16(addr)) << 16) | debugPeek16(addr + 2);
}

u16 Bus::read16(u32 addr, int partialCycles) {
    const int cpuPartialCycles = partialCycles;
    partialCycles += m68k ? m68k->getMemoryTimingOffset() : 0;
    addr &= 0xFFFFFF;

    // VDP needs special handling - single 16-bit read
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return 0xFFFF;
        if (m68k) m68k->applyRefreshFreeAccess();
        vdp->syncToCpuTimingOffset(partialCycles);
        if (m68k && vdpPortWaitDiagEnabled()) {
            m68k->addBusWaitCycles(1);
        }
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                return vdp->readData();
            case 2: case 3:  // Control port (status)
                return vdp->readControl();
            case 4: case 5: case 6: case 7:  // HV counter
                return vdp->readHVCounter();
            default:
                return 0xFFFF;
        }
    }

    // ROM / SRAM
    if (addr < 0x400000) {
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) ||
             (sramMapped && addr >= 0x200000))) {
            return cartridge->readSRAM16(addr);
        }

        const u32 bank = addr >> 19;
        const u32 bankOffset = addr & 0x7FFFF;
        const u32 mappedAddr = (static_cast<u32>(romBankRegs[bank]) << 19) | bankOffset;
        if (bankOffset < 0x7FFFF && mappedAddr + 1 < romSize) {
            return readBE16(rom, mappedAddr);
        }
        return (read8(addr, cpuPartialCycles) << 8) | read8(addr + 1, cpuPartialCycles);
    }

    // RAM
    if (addr >= 0xFF0000) {
        const u32 ramOffset = addr & 0xFFFF;
        return (static_cast<u16>(ram[ramOffset]) << 8) | ram[(ramOffset + 1) & 0xFFFF];
    }

    // Z80 area
    // 68K Z80-area word reads are modeled as one bus transaction.
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            addM68KZ80AreaWaitCycles();
            if (!(z80BusRequested || z80Reset)) {
                return 0xFFFF;
            }
            const u16 z80Addr = addr & 0x1FFF;
            return (static_cast<u16>(z80Ram[z80Addr]) << 8) | z80Ram[(z80Addr + 1) & 0x1FFF];
        }
        if (addr >= 0xA04000 && addr < 0xA04004) {
            addM68KZ80AreaWaitCycles();
            const u8 value = ym2612 ? ym2612->readStatus(static_cast<int>(addr & 3)) : 0;
            return (static_cast<u16>(value) << 8) | value;
        }
    }

    return (read8(addr, cpuPartialCycles) << 8) | read8(addr + 1, cpuPartialCycles);
}

void Bus::write8(u32 addr, u8 val) {
    int partialCycles = m68k ? m68k->state.cycles : 0;
    write8(addr, val, partialCycles);
}

void Bus::write8(u32 addr, u8 val, int partialCycles) {
    const int cpuPartialCycles = partialCycles;
    partialCycles += m68k ? m68k->getMemoryTimingOffset() : 0;
    addr &= 0xFFFFFF;

    // SRAM writes (0x200000-0x3FFFFF when mapped)
    if (addr >= 0x200000 && addr < 0x400000) {
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) || sramMapped)) {
            cartridge->writeSRAM8(addr, val);
        }
        return;
    }

    // RAM
    if (addr >= 0xFF0000) {
        ram[addr & 0xFFFF] = val;
        if (ramWriteWatchMatches(addr)) {
            std::fprintf(stderr,
                         "[RAMW8] frame=%d tc=%llu partial=%d addr=%06X val=%02X pc=%06X lastpc=%06X ln=%d\n",
                         m68k ? m68k->traceFrameNumber_ : 0,
                         static_cast<unsigned long long>(m68k ? (m68k->getTotalCycles() + static_cast<u64>(cpuPartialCycles)) : 0),
                         cpuPartialCycles,
                         addr,
                         val,
                         m68k ? m68k->getPC() & 0xFFFFFF : 0,
                         m68k ? m68k->getLastPC() & 0xFFFFFF : 0,
                         vdp ? vdp->getScanline() : -1);
        }
        return;
    }
    
    // Z80 area
    // 68K Z80-area IO incurs one M68K wait cycle per transaction.
    if (addr >= 0xA00000 && addr < 0xA10000) {
        addM68KZ80AreaWaitCycles();
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
        bool newRequest = (val & 1) != 0;
        // Hardware latency: BUSACK assertion takes ~3 Z80 cycles after
        // BUSREQ rises, and Z80 resumes ~1 Z80 cycle after BUSREQ falls.
        // We track elapsed Z80 cycles via z80CycleCounter (advanced from
        // Genesis::runZ80Burst after each z80.execute()) and settle
        // z80BusAck in updateBusAck(). Record the transition point here;
        // updateBusAck does the threshold check on subsequent ticks.
        if (newRequest && !z80BusRequested) {
            busreqAssertCycle = z80CycleCounter;
            busreqAssertMclk = m68kAccessMclk(cpuPartialCycles);
            z80BusAck = false;
            // z80BusAck stays false until updateBusAck sees >= 3 Z80 cycles.
        } else if (!newRequest && z80BusRequested) {
            busreqDeassertCycle = z80CycleCounter;
            busreqDeassertMclk = m68kAccessMclk(cpuPartialCycles);
            // z80BusAck stays true until updateBusAck sees >= 1 Z80 cycle.
        }
        z80BusRequested = newRequest;
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
        if (m68k) m68k->applyRefreshFreeAccess();
        if (fifoTimingLogEnabled()) {
            std::fprintf(stderr,
                         "[VDP-W8] addr=%06X val=%02X busPartialCyc=%d m68kCyc=%d memOff=%d pc=%06X ln=%d a6=%08X\n",
                         addr, val, partialCycles,
                         m68k ? m68k->state.cycles : -1,
                         m68k ? m68k->getMemoryTimingOffset() : -1,
                         m68k ? m68k->getPC() & 0xFFFFFF : 0,
                         vdp ? vdp->getScanline() : -1,
                         m68k ? m68k->state.a[6] : 0);
        }
        vdp->syncToCpuTimingOffset(partialCycles);
        if (m68k && vdpPortWaitDiagEnabled()) {
            m68k->addBusWaitCycles(1);
        }
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                waitForVDPFIFOForWrite();
                vdp->writeDataByte(val, (addr & 1) != 0);
                break;
            case 2: case 3:  // Control port
                vdp->writeControl((val << 8) | val);
                waitForVDP68KDMA();
                break;
            default:
                break;
        }
        return;
    }
}

void Bus::write16(u32 addr, u16 val) {
    int partialCycles = m68k ? m68k->state.cycles : 0;
    write16(addr, val, partialCycles);
}

void Bus::write16(u32 addr, u16 val, int partialCycles) {
    const int cpuPartialCycles = partialCycles;
    partialCycles += m68k ? m68k->getMemoryTimingOffset() : 0;
    addr &= 0xFFFFFF;

    // VDP word writes need special handling
    if (addr >= 0xC00000 && addr < 0xC00020) {
        if (!vdp) return;
        if (m68k) m68k->applyRefreshFreeAccess();
        if (fifoTimingLogEnabled()) {
            std::fprintf(stderr,
                         "[VDP-W16] addr=%06X val=%04X busPartialCyc=%d m68kCyc=%d memOff=%d pc=%06X ln=%d a6=%08X\n",
                         addr, val, partialCycles,
                         m68k ? m68k->state.cycles : -1,
                         m68k ? m68k->getMemoryTimingOffset() : -1,
                         m68k ? m68k->getPC() & 0xFFFFFF : 0,
                         vdp ? vdp->getScanline() : -1,
                         m68k ? m68k->state.a[6] : 0);
        }
        vdp->syncToCpuTimingOffset(partialCycles);
        if (m68k && vdpPortWaitDiagEnabled()) {
            m68k->addBusWaitCycles(1);
        }
        int reg = (addr & 0x1E) >> 1;
        switch (reg) {
            case 0: case 1:  // Data port
                waitForVDPFIFOForWrite();
                vdp->writeData(val);
                break;
            case 2: case 3:  // Control port
                vdp->writeControl(val);
                waitForVDP68KDMA();
                break;
            default:
                break;
        }
        return;
    }

    // SRAM writes
    if (addr >= 0x200000 && addr < 0x400000) {
        if (cartridge && cartridge->hasSRAM() &&
            (cartridge->isDirectMappedSRAMAddress(addr) || sramMapped)) {
            cartridge->writeSRAM16(addr, val);
        }
        return;
    }

    // RAM
    if (addr >= 0xFF0000) {
        writeBE16(ram, addr & 0xFFFF, val);
        if (ramWriteWatchMatches(addr) || ramWriteWatchMatches(addr + 1)) {
            std::fprintf(stderr,
                         "[RAMW16] frame=%d tc=%llu partial=%d addr=%06X val=%04X pc=%06X lastpc=%06X ln=%d "
                         "d0=%08X d1=%08X d2=%08X d3=%08X d4=%08X a0=%08X a1=%08X sr=%04X\n",
                         m68k ? m68k->traceFrameNumber_ : 0,
                         static_cast<unsigned long long>(m68k ? (m68k->getTotalCycles() + static_cast<u64>(cpuPartialCycles)) : 0),
                         cpuPartialCycles,
                         addr,
                         val,
                         m68k ? m68k->getPC() & 0xFFFFFF : 0,
                         m68k ? m68k->getLastPC() & 0xFFFFFF : 0,
                         vdp ? vdp->getScanline() : -1,
                         m68k ? m68k->state.d[0] : 0,
                         m68k ? m68k->state.d[1] : 0,
                         m68k ? m68k->state.d[2] : 0,
                         m68k ? m68k->state.d[3] : 0,
                         m68k ? m68k->state.d[4] : 0,
                         m68k ? m68k->state.a[0] : 0,
                         m68k ? m68k->state.a[1] : 0,
                         m68k ? m68k->state.sr : 0);
        }
        return;
    }

    // Z80 area
    // 68K Z80-area word writes are modeled as one bus transaction.
    if (addr >= 0xA00000 && addr < 0xA10000) {
        if (addr < 0xA02000) {
            addM68KZ80AreaWaitCycles();
            if (z80BusRequested || z80Reset) {
                const u16 z80Addr = addr & 0x1FFF;
                z80Ram[z80Addr] = static_cast<u8>(val >> 8);
                z80Ram[(z80Addr + 1) & 0x1FFF] = static_cast<u8>(val & 0xFF);
            }
            return;
        }
        if (addr >= 0xA04000 && addr < 0xA04004) {
            addM68KZ80AreaWaitCycles();
            if (ym2612) {
                const int port = (addr >> 1) & 1;
                if (addr & 1) {
                    ym2612->writeData(static_cast<u8>(val >> 8), port);
                } else {
                    ym2612->writeAddress(static_cast<u8>(val >> 8), port);
                }
            }
            return;
        }
    }

    // Other addresses use byte writes
    write8(addr, val >> 8, cpuPartialCycles);
    write8(addr + 1, val & 0xFF, cpuPartialCycles);
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
        if (genesis) genesis->syncYMBeforeRead();
        u8 status = ym2612 ? ym2612->readStatus(static_cast<int>(addr & 3)) : 0;
        if (genesis) {
            genesis->traceZ80YMRead(addr, status);
        }
        return status;
    }
    
    // Bank register: $6000-$60FF
    if (addr >= 0x6000 && addr < 0x6100) {
        return 0; // Write-only
    }
    
    // PSG: $7F00-$7FFF (directly addressable only from 68K)
    
    // 68K bus access: $8000-$FFFF (32KB window into 68K address space)
    // Z80 bank access adds 3 Z80 cycles (~18 master clocks) per access
    // and stalls the M68K bus by about 8 M68K cycles.
    if (addr >= 0x8000) {
        addZ80BankAccessPenalty();
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
        // Sync YM to current Z80 cycle before register write.
        // This generates samples with the OLD register values up to now,
        // so the write takes effect at the correct YM operator slot.
        if (genesis) genesis->syncYMBeforeWrite();
        if (genesis) genesis->traceZ80YMWrite(addr, val, (addr & 1) != 0);
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
    if (addr >= 0x7F00 && addr < 0x8000) {
        if (genesis) genesis->traceZ80PSGWrite(val);
        if (psg) psg->write(val);
        return;
    }
    
    // 68K bus access: $8000-$FFFF
    // Same 3 Z80 cycle penalty as reads
    if (addr >= 0x8000) {
        addZ80BankAccessPenalty();
        u32 addr68k = (z80Bank << 15) | (addr & 0x7FFF);
        write8(addr68k, val);
    }
}
