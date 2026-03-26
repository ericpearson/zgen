// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"
#include "video/hvc_tables.h"
#include "memory/bus.h"
#include "debug_flags.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
inline u32 expandChannel3To8(int value) {
    return (value << 5) | (value << 2) | (value >> 1);
}

inline int defaultVblankIrqAssertCycle(int activeWidth) {
    // Reference implementations and hardware-facing docs place VINT well into
    // the first VBlank line, not immediately after line start.
    // GPGX uses 788 MCLK in H40 and 770 MCLK in H32.
    const int mclk = (activeWidth == 320) ? 788 : 770;
    return (mclk + 6) / 7;
}

inline int vblankIrqDelayCycles(int activeWidth) {
    const char* env = std::getenv("GENESIS_VINT_DELAY_CYCLES");
    if (!env) {
        return defaultVblankIrqAssertCycle(activeWidth);
    }
    int value = std::atoi(env);
    return value >= 0 ? value : defaultVblankIrqAssertCycle(activeWidth);
}

inline u8 vCounterNtsc(int scanline) {
    return static_cast<u8>(scanline <= 0xEA ? scanline : scanline - 6);
}

inline u8 vCounterPal(int scanline, int activeHeight) {
    if (activeHeight >= 240) {
        if (scanline <= 0xFF) return static_cast<u8>(scanline);
        if (scanline <= 0x10A) return static_cast<u8>(scanline - 0x100);
        return static_cast<u8>(scanline - 0x39);
    }

    if (scanline <= 0xFF) return static_cast<u8>(scanline);
    if (scanline <= 0x102) return static_cast<u8>(scanline - 0x100);
    return static_cast<u8>(scanline - 0x39);
}

inline int firstM68kCycleForSlot(const VDPSlotTable& table, int slot) {
    if (slot <= 0) {
        return 0;
    }
    for (int cyc = 0; cyc < VDP_MAX_M68K_CYCLES; ++cyc) {
        if (table.slotAtM68kCycle[cyc] >= slot) {
            return cyc;
        }
    }
    return VDP_MAX_M68K_CYCLES - 1;
}

inline bool megaTimingEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = std::getenv("GENESIS_LOG_MEGA_TIMING");
        enabled = (g_debugMode && env && std::atoi(env) != 0) ? 1 : 0;
    }
    return enabled != 0;
}

inline int megaTimingFrameFirst() {
    static int value = []() {
        const char* env = std::getenv("GENESIS_LOG_MEGA_FRAME_FIRST");
        return env ? std::atoi(env) : 996;
    }();
    return value;
}

inline int megaTimingFrameLast() {
    static int value = []() {
        const char* env = std::getenv("GENESIS_LOG_MEGA_FRAME_LAST");
        return env ? std::atoi(env) : 1002;
    }();
    return value;
}

inline bool megaTimingShouldLog(int frame, int scanline, int activeHeight) {
    if (!megaTimingEnabled()) {
        return false;
    }
    if (frame < megaTimingFrameFirst() || frame > megaTimingFrameLast()) {
        return false;
    }
    return scanline == activeHeight || (scanline >= 246 && scanline <= 255);
}

inline u16 megaTimingRamWord(const Bus* bus, u32 addr) {
    if (!bus) {
        return 0;
    }
    const u8* ram = bus->getRam();
    const u32 offset = addr & 0xFFFF;
    return static_cast<u16>((ram[offset] << 8) | ram[(offset + 1) & 0xFFFF]);
}

}

VDP::VDP() : bus(nullptr), videoStandard_(VideoStandard::NTSC) {
    reset();
}

void VDP::megaTimingLog(const char* tag) const {
    if (!megaTimingShouldLog(debugFrameNumber_, scanline, activeHeight)) {
        return;
    }
    std::fprintf(stderr,
                 "[MT] frame=%d tag=%s ln=%d cyc=%d hc=%02X hpend=%d vpend=%d varmed=%d vassert=%d hblank=%d FF0004=%04X FF04B4=%04X FF04E4=%04X FF07BE=%04X FF116C=%04X\n",
                 debugFrameNumber_,
                 tag,
                 scanline,
                 lineCycles,
                 hblankCounter & 0xFF,
                 hblankPending() ? 1 : 0,
                 vblankIRQ ? 1 : 0,
                 vblankIRQArmed ? 1 : 0,
                 vblankIRQAssertCycle,
                 inHBlankPeriod() ? 1 : 0,
                 megaTimingRamWord(bus, 0xFF0004),
                 megaTimingRamWord(bus, 0xFF04B4),
                 megaTimingRamWord(bus, 0xFF04E4),
                 megaTimingRamWord(bus, 0xFF07BE),
                 megaTimingRamWord(bus, 0xFF116C));
}

void VDP::reset() {
    const VideoStandard preservedStandard = videoStandard_;

    std::memset(vram, 0, sizeof(vram));
    std::memset(cram, 0, sizeof(cram));
    std::memset(vsram, 0, sizeof(vsram));
    std::memset(vsramSnapshot, 0, sizeof(vsramSnapshot));
    vsramSnapshotTaken = false;
    std::memset(hscrollSnapshot, 0, sizeof(hscrollSnapshot));
    std::memset(regs, 0, sizeof(regs));
    std::memset(framebuffer, 0, sizeof(framebuffer));
    std::memset(fifo, 0, sizeof(fifo));

    controlPending = false;
    controlLatch = 0;
    code = 0;
    address = 0;
    cachedControlWritePending = false;
    cachedControlWriteValue = 0;

    dmaActive = false;
    dmaMode = 0;
    dmaWordsRemaining = 0;
    dmaFillPending = false;
    dmaFillValue = 0;
    dmaFillCode = 0;
    dmaVramSecondSlot = false;
    dmaCopySecondSlot = false;
    dmaAddress = 0;
    fifoCount = 0;
    fifoReadIndex = 0;
    fifoWriteIndex = 0;
    vramWriteSecondSlot = false;
    std::memset(displayEnableEvents, 0, sizeof(displayEnableEvents));
    displayEnableEventCount = 0;
    displayEnableEventReadIndex = 0;
    displayEnableEventWriteIndex = 0;
    cramDotCount = 0;
    scanline = 0;
    hcounter = 0;
    lineCycles = 0;
    lineCycleBudget = VDP_MAX_M68K_CYCLES;
    currentSlotIndex = -1;
    hblankStartCycle_ = 0;
    hblankCounterDecremented_ = false;
    lineIsBlank = true;
    oddFrame = false;
    renderedPixels = 0;

    vblankIRQ = false;
    vblankFlag = false;
    vblankIRQArmed = false;
    vblankIRQAssertCycle = 0;
    hblankIRQ = false;
    hblankCounter = 0xFF;

    spriteCollision = false;
    spriteOverflow = false;
    spriteLineCount[0] = 0;
    spriteLineCount[1] = 0;
    spriteLineValid[0] = false;
    spriteLineValid[1] = false;
    spriteLineNumber[0] = -1;
    spriteLineNumber[1] = -1;
    debugFrameNumber_ = 0;

    hvLatch = 0;
    hvLatched = false;

    // Common power-on increment value used by most software.
    regs[0x0F] = 2;

    // Defaults for decoded state.
    activeWidth = 320;
    activeHeight = 224;
    displayEnabled = false;
    displayEnabledLatch = false;
    displayEnableApplySerial_ = 0;
    lastDisplayEnableApplyScanline_ = -1;
    lastDisplayEnableApplyPixel_ = 0;
    lastDisplayEnableApplyValue_ = false;
    vblankEnabled = false;
    hblankEnabled = false;
    dmaEnabled = false;
    shadowHighlightEnabled = false;
    interlaceMode = 0;
    interlaceModeLatch = 0;
    videoStandard_ = preservedStandard;
    scrollABase = 0;
    scrollBBase = 0;
    windowBase = 0;
    spriteBase = 0;
    hscrollBase = 0;
    hscrollMode = 0;
    vscrollMode = 0;
    bgColorIndex = 0;
    scrollWidth = 32;
    scrollHeight = 32;

    updateRegisters();
    refreshCachedColors();
}

void VDP::updateRegisters() {
    for (int i = 0; i < 24; i++) {
        decodeRegister(i);
    }
}

void VDP::drawCurrentLine() {
    renderScanlineSegment(scanline, 0, activeWidth);
}

void VDP::writeControl(u16 value) {
    static const bool logHintTiming = []() {
        if (!g_debugMode) return false;
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    // If the first half of a longword control-port write started a 68K-bus
    // DMA, the second control word is not observed until the DMA completes.
    // Formula One depends on this ordering.
    if (!controlPending && dmaActive && dmaMode < 2) {
        cachedControlWritePending = true;
        cachedControlWriteValue = value;
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[CTRL-CACHE] ln=%d cyc=%d val=%04X dmaMode=%d dmaRem=%u\n",
                         scanline,
                         lineCycles,
                         value,
                         dmaMode,
                         dmaWordsRemaining);
        }
        return;
    }

    // Register write (single word): 10RR RRRR DDDD DDDD
    // Always recognized regardless of controlPending state (real hardware behavior).
    if ((value & 0xC000) == 0x8000) {
        controlPending = false;
        // Register writes update address[13:0] and code[1:0] from the written
        // word, just like the first half of a two-word command (confirmed by
        // hardware tests). Since bits 15:14 are always 10 for
        // register writes, code[1:0] becomes 2 — an invalid write target.
        // Data port writes therefore become no-ops until a new control-port
        // command sets a valid target (1=VRAM, 3=CRAM, 5=VSRAM).
        address = (address & 0xC000) | (value & 0x3FFF);
        code = (code & 0x3C) | static_cast<u8>((value >> 14) & 0x03);
        int reg = (value >> 8) & 0x1F;
        if (reg < 24) {
            const u8 oldRegValue = regs[reg];
            flushCurrentLineToCurrentCycle();
            regs[reg] = value & 0xFF;
            decodeRegister(reg);
            if (logHintTiming && (reg == 0x01 || reg == 0x02 || reg == 0x07)) {
                std::fprintf(stderr,
                             "[REG%02X] ln=%d cyc=%d px=%d val=%02X old=%02X dispEn=%d bg=%02X rendPx=%d\n",
                             reg,
                             scanline,
                             lineCycles,
                             currentVisiblePixel(),
                             regs[reg],
                             oldRegValue,
                             displayEnabled ? 1 : 0,
                             regs[0x07] & 0x3F,
                             renderedPixels);
            }
            if (reg == 0x01) {
                const bool oldDisplayEnabled = (oldRegValue & 0x40) != 0;
                const bool newDisplayEnabled = (regs[0x01] & 0x40) != 0;
                if (oldDisplayEnabled != newDisplayEnabled) {
                    scheduleDisplayEnableChange(newDisplayEnabled);
                }
            }
        }
        return;
    }

    if (!controlPending) {
        controlLatch = value;
        // First word partially updates address A13:A0 and code CD1:CD0 immediately
        address = (address & 0xC000) | (value & 0x3FFF);
        code = (code & 0x3C) | static_cast<u8>((value >> 14) & 0x03);
        controlPending = true;
        return;
    }

    controlPending = false;

    // Two-word command decode.
    // Address bits: A13..A0 in first word, A15..A14 in second word bits 1..0.
    address = (controlLatch & 0x3FFF) | ((value & 0x0003) << 14);

    // Code bits: CD1..CD0 in first word bits 15..14, CD5..CD2 in second word bits 7..4.
    code = static_cast<u8>(((controlLatch >> 14) & 0x03) | ((value >> 2) & 0x3C));

    if ((code & 0x20) && dmaEnabled) {
        executeDMA();
    }
}

u16 VDP::readControl() {
    return readControl(0);
}

u16 VDP::readControl(int partialCycles) {
    controlPending = false;

    u16 status = 0x3400; // Fixed bits in mode 5.

    // FIFO status — project count forward by partialCycles for accuracy.
    // On real hardware, FIFO flags reflect the exact moment of the bus read;
    // entries may have drained at external slots since the last clockM68K().
    int projectedFifo = (partialCycles > 0)
        ? fifoCountAtCycleOffset(partialCycles)
        : fifoCount;
    if (projectedFifo == 0) {
        status |= 0x0200;  // FIFO empty
    }
    if (projectedFifo >= FIFO_SIZE) {
        status |= 0x0100;  // FIFO full
    }

    // Bit 7: F flag — "VINT happened", set at VBlank start, cleared by
    // status read.  This is a separate latch from the interrupt pending
    // signal (vblankIRQ) which drives /VINT to the 68K and is only cleared
    // by interrupt acknowledgment.  Games that read status inside an H-int
    // handler that spans the VBlank boundary (e.g. Sonic 2 Chemical Plant
    // Zone water effect) depend on V-int remaining pending after the read.
    // Reference: Nemesis VDP internals, BlastEm vdp_control_port_read.
    if (vblankFlag) {
        status |= 0x0080;
    }

    // Sprite overflow (max sprites per line exceeded)
    if (spriteOverflow) {
        status |= 0x0040;
    }

    // Sprite collision
    if (spriteCollision) {
        status |= 0x0020;
    }

    if (oddFrame) {
        status |= 0x0010;
    }

    if (scanline >= activeHeight) {
        status |= 0x0008;
    }

    // Cycle-accurate HBlank status flag using fixed master-clock windows
    // derived from verified hardware timings.
    // H40: mclk 228-872, H32: mclk 280-860, relative to line start.
    // Convert to M68K cycles: divide by 7 (master clock divider).
    // Reference values come from verified hardware timings.
    {
        int effectiveCycle = lineCycles + partialCycles;
        if (effectiveCycle >= VDP_MAX_M68K_CYCLES) {
            effectiveCycle -= VDP_MAX_M68K_CYCLES;
        }
        int hblankStart, hblankEnd;
        if (activeWidth == 320) {
            hblankStart = 228 / 7;  // 32 M68K cycles
            hblankEnd   = 872 / 7;  // 124 M68K cycles
        } else {
            hblankStart = 280 / 7;  // 40 M68K cycles
            hblankEnd   = 860 / 7;  // 122 M68K cycles
        }
        if (effectiveCycle >= hblankStart && effectiveCycle < hblankEnd) {
            status |= 0x0004;
        }
    }

    if (dmaActive) {
        status |= 0x0002;
    }

    // Status reads clear the F flag and sprite collision/overflow latches.
    // The interrupt pending signals (vblankIRQ, hblankIRQ) are NOT cleared
    // — on real hardware, /VINT and /HINT are only de-asserted when the
    // 68K acknowledges the interrupt via the IACK cycle.
    vblankFlag = false;
    spriteCollision = false;
    spriteOverflow = false;

    return status;
}

void VDP::writeData(u16 value) {
    controlPending = false;

    // Fill DMA: capture fill byte but let the normal write path execute
    // first.  On real hardware the data-port write that triggers a fill
    // also performs a normal VRAM/CRAM/VSRAM write before the fill begins.
    bool startFill = false;
    if (dmaFillPending) {
        dmaFillValue = value;
        dmaFillPending = false;
        startFill = true;
    }

    // Normal write path (executes even when triggering a fill)
    int target = code & 0x0F;
    if (target == 1 || target == 3 || target == 5) {
        if (!enqueueFIFOEntry(value, address, code, false, false)) {
            applyFIFOEntry({value, address, code, false, false});
        }
    }

    address = static_cast<u16>(address + regs[15]);

    // Start fill DMA after the initial write + auto-increment
    if (startFill) {
        dmaActive = true;
        dmaMode = 2;
        dmaFillCode = code;  // Latch target — 68K keeps running during fill
        dmaAddress = address;  // Fill begins from the post-increment address
        dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
        if (dmaWordsRemaining == 0) dmaWordsRemaining = 0x10000;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMAFILL-START] ln=%d cyc=%d addr=%04X len=%u inc=%02X fill=%04X code=%02X\n",
                         scanline,
                         lineCycles,
                         dmaAddress,
                         dmaWordsRemaining,
                         regs[15],
                         dmaFillValue,
                         dmaFillCode);
        }
    }
}

void VDP::writeDataByte(u8 value, bool lowByte) {
    controlPending = false;

    // Fill DMA can be triggered by byte writes too (the fill value is the
    // written byte).  The game may use write8 to the data port instead of
    // write16 to start a fill.
    bool startFill = false;
    if (dmaFillPending) {
        dmaFillValue = (static_cast<u16>(value) << 8) | value;
        dmaFillPending = false;
        startFill = true;
    }

    int target = code & 0x0F;
    if (target == 1 || target == 3 || target == 5) {
        if (!enqueueFIFOEntry(value, address, code, true, lowByte)) {
            applyFIFOEntry({value, address, code, true, lowByte});
        }
    }

    address = static_cast<u16>(address + regs[15]);

    if (startFill) {
        dmaActive = true;
        dmaMode = 2;
        dmaFillCode = code;  // Latch target — 68K keeps running during fill
        dmaAddress = address;
        dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
        if (dmaWordsRemaining == 0) dmaWordsRemaining = 0x10000;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMAFILL-START] ln=%d cyc=%d addr=%04X len=%u inc=%02X fill=%04X code=%02X\n",
                         scanline,
                         lineCycles,
                         dmaAddress,
                         dmaWordsRemaining,
                         regs[15],
                         dmaFillValue,
                         dmaFillCode);
        }
    }
}

u16 VDP::readData() {
    controlPending = false;

    u16 value = 0;
    int target = code & 0x0F;

    switch (target) {
        case 0: // VRAM
            value = (static_cast<u16>(vram[address & 0xFFFF]) << 8) |
                    vram[(address + 1) & 0xFFFF];
            break;

        case 4: // VSRAM
            if ((address >> 1) < 40) {
                value = vsram[(address >> 1) & 0x3F];
            }
            break;

        case 8: // CRAM
            value = cram[(address >> 1) & 0x3F];
            break;

        default:
            break;
    }

    address = static_cast<u16>(address + regs[15]);
    return value;
}

u16 VDP::readHVCounter() {
    // If HV counter is latched (reg 0x00 bit 1), return latched value
    if (hvLatched) {
        return hvLatch;
    }

    const u8 vcounter = (videoStandard_ == VideoStandard::PAL)
                            ? vCounterPal(scanline, activeHeight)
                            : vCounterNtsc(scanline);
    // Use per-master-clock H counter tables for cycle-accurate reads.
    // This matches GPGX's hctab[cycles % MCYCLES_PER_LINE] approach and
    // fixes games that read the HV counter mid-slot (Panorama Cotton,
    // Skitchin, Lotus 2, Dashin Desperados).
    int mclk = lineCycles * 7;
    if (mclk >= 3420) mclk = 3419;
    if (mclk < 0) mclk = 0;
    u8 hcounterVal = (activeWidth == 320) ? cycle2hc40[mclk] : cycle2hc32[mclk];
    return (static_cast<u16>(vcounter) << 8) | hcounterVal;
}

void VDP::setVideoStandard(VideoStandard standard) {
    videoStandard_ = standard;
}

int VDP::getTotalScanlines() const {
    return videoStandard_ == VideoStandard::PAL ? 313 : 262;
}

void VDP::decodeRegister(int reg) {
    // Temporary: log H-int enable and display enable changes
    {
        static const bool logRegChanges = []() {
            const char* e = std::getenv("GENESIS_LOG_REG_CHANGES");
            return e && e[0] != '0';
        }();
        if (logRegChanges && (reg == 0x00 || reg == 0x01)) {
            std::fprintf(stderr, "[REG%02X] ln=%d cyc=%d val=0x%02X hint_en=%d disp_en=%d\n",
                         reg, scanline, lineCycles, regs[reg],
                         (regs[0] >> 4) & 1, (regs[1] >> 6) & 1);
        }
    }
    switch (reg) {
        case 0x00:
            hblankEnabled = (regs[0x00] & 0x10) != 0;
            // HV counter latch: bit 1
            if (regs[0x00] & 0x02) {
                if (!hvLatched) {
                    hvLatch = readHVCounter();
                    hvLatched = true;
                }
            } else {
                hvLatched = false;
            }
            break;

        case 0x01:
            displayEnabledLatch = (regs[0x01] & 0x40) != 0;
            vblankEnabled = (regs[0x01] & 0x20) != 0;
            dmaEnabled = (regs[0x01] & 0x10) != 0;
            activeHeight = (regs[0x01] & 0x08) ? 240 : 224;
            break;

        case 0x02:
            scrollABase = (regs[0x02] & 0x38) << 10;
            break;

        case 0x03:
            // H40 forces WD11 low.
            windowBase = (regs[0x03] & (activeWidth == 320 ? 0x3C : 0x3E)) << 10;
            break;

        case 0x04:
            scrollBBase = (regs[0x04] & 0x07) << 13;
            break;

        case 0x05:
            // H40 forces AT9 low.
            spriteBase = (regs[0x05] & (activeWidth == 320 ? 0x7E : 0x7F)) << 9;
            break;

        case 0x07:
            bgColorIndex = regs[0x07] & 0x3F;
            break;

        case 0x0A:
            // Only stores the reload value; the running counter is not affected.
            // Charles MacDonald: "The counter is not loaded when register #10 is written to."
            break;

        case 0x0B:
            vscrollMode = (regs[0x0B] >> 2) & 0x01;
            hscrollMode = regs[0x0B] & 0x03;
            break;

        case 0x0C:
            // H40 when RS1/RS0 are both set.
            activeWidth = ((regs[0x0C] & 0x81) == 0x81) ? 320 : 256;
            // Shadow/Highlight: bit 3
            shadowHighlightEnabled = (regs[0x0C] & 0x08) != 0;
            interlaceMode = (regs[0x0C] >> 1) & 0x03;
            interlaceModeLatch = interlaceMode;
            // Width mode affects base masks for these tables.
            windowBase = (regs[0x03] & (activeWidth == 320 ? 0x3C : 0x3E)) << 10;
            spriteBase = (regs[0x05] & (activeWidth == 320 ? 0x7E : 0x7F)) << 9;
            break;

        case 0x0D:
            hscrollBase = (regs[0x0D] & 0x3F) << 10;
            break;

        case 0x10: {
            int hsz = regs[0x10] & 0x03;
            int vsz = (regs[0x10] >> 4) & 0x03;

            scrollWidth = (hsz == 1) ? 64 : (hsz == 3) ? 128 : 32;
            scrollHeight = (vsz == 1) ? 64 : (vsz == 3) ? 128 : 32;
            break;
        }

        default:
            break;
    }
}

void VDP::beginScanlineCommon() {
    static const bool logHintTiming = []() {
        if (!g_debugMode) return false;
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    lineCycles = 0;
    currentSlotIndex = -1;  // No slot processed yet; first clockM68K processes from slot 0
    hblankCounterDecremented_ = false;
    renderedPixels = 0;
    cramDotCount = 0;
    // VSRAM snapshot is deferred: taken at pixel-output start (latchVscroll)
    // so the H-int handler has time to write new scroll values first.
    vsramSnapshotTaken = false;

    // Latch HSCROLL values at line start. On real hardware, the VDP reads
    // HSCROLL data from VRAM at the first access slot of each scanline,
    // before pattern fetching begins. Mid-line VRAM writes to the HSCROLL
    // table do not affect the current line's scroll position.
    {
        int line = scanline;
        int hoff = hscrollBase;
        switch (hscrollMode) {
            case 2: hoff += (line & ~7) * 4; break;
            case 3: hoff += line * 4; break;
            default: break;
        }
        hscrollSnapshot[0] = (static_cast<u16>(vram[hoff & 0xFFFF]) << 8)
                           | vram[(hoff + 1) & 0xFFFF];
        hscrollSnapshot[1] = (static_cast<u16>(vram[(hoff + 2) & 0xFFFF]) << 8)
                           | vram[(hoff + 3) & 0xFFFF];
    }

    // Process pending display-enable events before determining slot table,
    // since display-disabled active lines need the blank table for correct
    // DMA bandwidth (VDP skips pattern fetches when reg 1 bit 6 is clear).
    while (displayEnableEventCount > 0) {
        const VDPDisplayEnableEvent& event = displayEnableEvents[displayEnableEventReadIndex];
        if (event.scanline > scanline) {
            break;
        }
        if (event.scanline == scanline && event.pixel > 0) {
            break;
        }
        displayEnabled = event.value;
        recordDisplayEnableApply(scanline, 0, displayEnabled);
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DISP-APPLY] ln=%d px=0 val=%d reason=line-start-queue\n",
                         scanline,
                         displayEnabled ? 1 : 0);
        }
        displayEnableEventReadIndex =
            (displayEnableEventReadIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
        displayEnableEventCount--;
    }

    // Rendering blankness is latched from the current display-enable state at
    // line start. Bandwidth scheduling uses live displayEnabled instead so a
    // late-HBlank display disable can free slots immediately for DMA/FIFO.
    lineIsBlank = (scanline >= activeHeight) || !displayEnabled;
    const VDPSlotTable& activeTable = (activeWidth == 320) ? kH40Active : kH32Active;
    hcounter = activeTable.hcounterAtSlot[0];

    // M68K cycle at which HBlank starts (for H-int timing). Precomputed in
    // the slot table via reverse lookup, avoiding ±1 truncation from div-by-7.
    hblankStartCycle_ = activeTable.hblankStartM68kCycle;

    if (scanline == 0) {
        // The H-int counter is reloaded once at the frame boundary, after the
        // last blank line completes. It is not reloaded on every VBlank line.
        hblankCounter = regs[0x0A];
    } else if (scanline == activeHeight && hblankCounter == 0) {
        // If the last active-line HBlank decremented the counter to zero, the
        // pending H-int becomes visible at the first VBlank line without an
        // additional reload.
        hblankIRQ = true;
        megaTimingLog("HINT-ASSERT-VBLANK");
    }
    // Active-line H-counter is decremented at HBlank start inside clockM68K()

    if (scanline == activeHeight) {
        vblankFlag = true;  // F flag always set at VBlank start (status bit 7)
    }
    refreshVBlankIRQState();
    megaTimingLog("LINE-START");

    // Sprite visibility is latched at line start. Snapshot the current
    // line's SAT entries now so end-of-line rendering does not see sprite
    // updates written later by this line's CPU work.
    if (scanline >= activeHeight) {
        for (int field = 0; field < 2; field++) {
            spriteLineCount[field] = 0;
            spriteLineValid[field] = false;
            spriteLineNumber[field] = -1;
        }
    } else {
        const int fieldCount = (interlaceMode == 3) ? 2 : 1;
        const bool savedOddFrame = oddFrame;
        for (int field = 0; field < fieldCount; field++) {
            if (interlaceMode == 3) {
                oddFrame = (field == 1);
            }
            parseSprites(scanline, spriteLineCache[field], spriteLineCount[field]);
            spriteLineValid[field] = true;
            spriteLineNumber[field] = scanline;
        }
        oddFrame = savedOddFrame;

        for (int field = fieldCount; field < 2; field++) {
            spriteLineCount[field] = 0;
            spriteLineValid[field] = false;
            spriteLineNumber[field] = -1;
        }
    }
}

void VDP::refreshVBlankIRQState() {
    if (scanline != activeHeight || !vblankEnabled) {
        vblankIRQArmed = false;
        vblankIRQAssertCycle = 0;
        megaTimingLog("VINT-DISARM");
        return;
    }

    if (vblankIRQ) {
        vblankIRQArmed = false;
        vblankIRQAssertCycle = 0;
        megaTimingLog("VINT-ALREADY-PENDING");
        return;
    }

    const int delayCycles = vblankIrqDelayCycles(activeWidth);
    if (delayCycles == 0) {
        vblankIRQ = true;
        vblankIRQArmed = false;
        vblankIRQAssertCycle = 0;
        megaTimingLog("VINT-IMMEDIATE");
        return;
    }

    if (lineCycles >= delayCycles) {
        vblankIRQ = true;
        vblankIRQArmed = false;
        vblankIRQAssertCycle = 0;
        megaTimingLog("VINT-LATE-ASSERT");
        return;
    }

    vblankIRQArmed = true;
    vblankIRQAssertCycle = delayCycles;
    megaTimingLog("VINT-ARM");
}

void VDP::beginScanline() {
    beginScanlineTimed();
}

void VDP::endScanline() {
    endScanlineTimed();

    scanline++;
    if (scanline >= getTotalScanlines()) {
        scanline = 0;
        oddFrame = !oddFrame;
    }
}

void VDP::acknowledgeInterrupt(int level) {
    megaTimingLog(level >= 6 ? "IACK-VINT" : "IACK-HINT");
    // Match the VDP's priority: V-INT is cleared before H-INT.
    if (level >= 6 && vblankIRQ) {
        vblankIRQ = false;
        return;
    }
    if (hblankIRQ) {
        hblankIRQ = false;
    }
}

const VDPSlotTable& VDP::currentSlotTable() const {
    const bool bandwidthBlank = (scanline >= activeHeight) || !effectiveDisplayEnabled();
    if (activeWidth == 320) {
        return bandwidthBlank ? kH40Blank : kH40Active;
    }
    return bandwidthBlank ? kH32Blank : kH32Active;
}

int VDP::slotIndexForM68kCycle(int cycle) const {
    const VDPSlotTable& table = currentSlotTable();
    if (cycle < 0) return 0;
    if (cycle >= VDP_MAX_M68K_CYCLES) return table.count - 1;
    return table.slotAtM68kCycle[cycle];
}

void VDP::processExternalSlot() {
    // FIFO has priority over DMA at external slots
    if (fifoCount > 0) {
        const int target = fifo[fifoReadIndex].code & 0x07;
        if (target == 1) {
            // VRAM writes take 2 external slots per word
            if (!vramWriteSecondSlot) {
                vramWriteSecondSlot = true;
                return;
            }
            vramWriteSecondSlot = false;
        }
        const VDPFifoEntry entry = fifo[fifoReadIndex];
        fifoReadIndex = (fifoReadIndex + 1) % FIFO_SIZE;
        fifoCount--;
        applyFIFOEntry(entry);
        return;
    }

    if (dmaActive && dmaWordsRemaining > 0) {
        processDMASlot();
    }
}

void VDP::latchVscroll() {
    if (vsramSnapshotTaken) return;
    vsramSnapshotTaken = true;
    std::memcpy(vsramSnapshot, vsram, sizeof(vsram));
}

void VDP::clockM68K(int cycles) {
    static const bool logHintTiming = []() {
        if (!g_debugMode) return false;
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    if (cycles <= 0) {
        return;
    }

    int lineLimit = lineCycleBudget;
    if (lineLimit <= 0) {
        lineLimit = VDP_MAX_M68K_CYCLES;
    }

    int cyclesRemaining = cycles;
    while (cyclesRemaining > 0 && lineCycles < lineLimit) {
        const int sliceStartCycle = lineCycles;
        int newLineCycles = lineCycles + cyclesRemaining;
        if (newLineCycles > lineLimit) {
            newLineCycles = lineLimit;
        }

        if (scanline < activeHeight && displayEnableEventCount > 0) {
            int index = displayEnableEventReadIndex;
            for (int remaining = displayEnableEventCount; remaining > 0; remaining--) {
                const VDPDisplayEnableEvent& event = displayEnableEvents[index];
                if (event.scanline > scanline) {
                    break;
                }
                if (event.scanline == scanline) {
                    int eventCycle = m68kCycleForVisiblePixel(event.pixel);
                    if (eventCycle > lineCycles && eventCycle < newLineCycles) {
                        newLineCycles = eventCycle;
                        break;
                    }
                }
                index = (index + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
            }
        }

        if (vblankIRQArmed && scanline == activeHeight && newLineCycles >= vblankIRQAssertCycle) {
            vblankIRQ = true;
            vblankIRQArmed = false;
            static const bool logHintTiming = []() {
                const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
                return env && std::atoi(env) != 0;
            }();
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[VINT-ASSERT] ln=%d cyc=%d newCyc=%d delay=%d\n",
                             scanline,
                             lineCycles,
                             newLineCycles,
                             vblankIRQAssertCycle);
            }
            megaTimingLog("VINT-ASSERT");
        }

        if (!hblankCounterDecremented_ && newLineCycles >= hblankStartCycle_ && scanline < activeHeight) {
            hblankCounterDecremented_ = true;
            if (hblankCounter == 0) {
                hblankCounter = regs[0x0A];
                hblankIRQ = true;
                if (logHintTiming) {
                    std::fprintf(stderr,
                                 "[HINT] ln=%d cyc=%d newCyc=%d hbStart=%d reg0A=%02X bgIdx=%02X dispEn=%d blank=%d dma=%d dmaMode=%d dmaRem=%u enabled=%d\n",
                                 scanline,
                                 lineCycles,
                                 newLineCycles,
                                 hblankStartCycle_,
                                 regs[0x0A],
                                 regs[0x07],
                                 displayEnabled ? 1 : 0,
                                 lineIsBlank ? 1 : 0,
                                 dmaActive ? 1 : 0,
                                 dmaMode,
                                 dmaWordsRemaining,
                                 hblankEnabled ? 1 : 0);
                }
                megaTimingLog("HINT-ASSERT");
            } else {
                hblankCounter--;
            }
        }

        // VSRAM latch is handled by flushCurrentLineToPixel(), gated on
        // lineCycles >= 112 to ensure the H-int handler has written.

        const VDPSlotTable& table = currentSlotTable();
        int targetSlot = table.slotAtM68kCycle[newLineCycles < VDP_MAX_M68K_CYCLES ? newLineCycles : VDP_MAX_M68K_CYCLES - 1];

        if ((fifoCount > 0 || (dmaActive && dmaWordsRemaining > 0)) && currentSlotIndex < targetSlot) {
            int first = currentSlotIndex + 1;
            if (first < 0) first = 0;
            int s = (first < table.count) ? table.nextExternalSlot[first] : table.count;
            while (s <= targetSlot && s < table.count) {
                currentSlotIndex = s;
                lineCycles = firstM68kCycleForSlot(table, s);
                const VDPSlotTable& activeTimingTable = (activeWidth == 320) ? kH40Active : kH32Active;
                hcounter = activeTimingTable.hcounterAtSlot[s < activeTimingTable.count
                                                                ? s
                                                                : activeTimingTable.count - 1];
                processExternalSlot();
                if (fifoCount == 0 && (!dmaActive || dmaWordsRemaining == 0)) {
                    break;
                }
                s = (s + 1 < table.count) ? table.nextExternalSlot[s + 1] : table.count;
            }
        }

        currentSlotIndex = targetSlot;
        lineCycles = newLineCycles;
        const VDPSlotTable& activeTimingTable = (activeWidth == 320) ? kH40Active : kH32Active;
        hcounter = activeTimingTable.hcounterAtSlot[targetSlot < activeTimingTable.count
                                                        ? targetSlot
                                                        : activeTimingTable.count - 1];

        const int consumedCycles = newLineCycles - sliceStartCycle;
        if (consumedCycles <= 0) {
            break;
        }
        cyclesRemaining -= consumedCycles;
    }
}

int VDP::currentVisiblePixel() const {
    if (scanline >= activeHeight) {
        return activeWidth;
    }
    if (currentSlotIndex < 0) {
        return 0;
    }
    // Match the viewport-relative model used for display-enable changes.
    // Verified hardware timings place bitmap X=0 later than the raw H=0x00
    // transition because the 320/256-wide viewport sits inside a wider
    // scanline. Formula One depends on this later origin.
    static constexpr int kViewportStartMclk = 860;
    int mclk = lineCycles * 7;
    const int dotDivisor = (activeWidth == 320) ? 8 : 10;

    if (mclk < kViewportStartMclk) return 0;
    int pixel = ((mclk - kViewportStartMclk) / dotDivisor) + 16;
    return (pixel > activeWidth) ? activeWidth : pixel;
}

int VDP::visiblePixelAtLineCycle(int cycle) const {
    if (scanline >= activeHeight) {
        return activeWidth;
    }
    static constexpr int kViewportStartMclk = 860;
    const int dotDivisor = (activeWidth == 320) ? 8 : 10;
    int mclk = cycle * 7;
    if (mclk < kViewportStartMclk) {
        return 0;
    }
    int pixel = ((mclk - kViewportStartMclk) / dotDivisor) + 16;
    return (pixel > activeWidth) ? activeWidth : pixel;
}

int VDP::m68kCycleForVisiblePixel(int pixel) const {
    if (pixel <= 0) {
        return 0;
    }
    static constexpr int kViewportStartMclk = 860;
    const int dotDivisor = (activeWidth == 320) ? 8 : 10;
    int effectivePixel = (pixel < 16) ? 16 : pixel;
    int mclk = kViewportStartMclk + ((effectivePixel - 16) * dotDivisor);
    int cycle = (mclk + 6) / 7;
    if (cycle >= VDP_MAX_M68K_CYCLES) {
        cycle = VDP_MAX_M68K_CYCLES - 1;
    }
    return cycle;
}

int VDP::nextVisibleSlotBoundaryPixel() const {
    if (scanline >= activeHeight) {
        return activeWidth;
    }
    static constexpr int kViewportStartMclk = 860;

    const VDPSlotTable& table = (activeWidth == 320) ? kH40Active : kH32Active;
    int nextSlot = currentSlotIndex + 1;
    if (nextSlot < 0) nextSlot = 0;
    if (nextSlot >= table.count) return activeWidth;

    int mclk = table.mclkOffset[nextSlot];
    const int dotDivisor = (activeWidth == 320) ? 8 : 10;

    if (mclk < kViewportStartMclk) return 0;

    int pixel = ((mclk - kViewportStartMclk) / dotDivisor) + 16;
    // Round up to next 2-pixel boundary for segment alignment.
    pixel = ((pixel + 1) / 2) * 2;
    return (pixel > activeWidth) ? activeWidth : pixel;
}

void VDP::flushCurrentLineToCurrentCycle() {
    flushCurrentLineToPixel(currentVisiblePixel());
}

void VDP::recordDisplayEnableApply(int applyScanline, int applyPixel, bool value) {
    lastDisplayEnableApplyScanline_ = applyScanline;
    lastDisplayEnableApplyPixel_ = applyPixel;
    lastDisplayEnableApplyValue_ = value;
    displayEnableApplySerial_++;
}

void VDP::flushCurrentLineToPixel(int pixelEnd) {
    if (scanline >= activeHeight) {
        renderedPixels = activeWidth;
        return;
    }
    // Latch VSRAM only after the H-int handler has had time to write new
    // scroll values (~cycle 90).  Gate on cycle 112 to match GPGX's timing
    // where render_line(N) runs after m68k_run(N-1) includes the handler.
    // Mid-line flushes before cycle 112 use the previous line's snapshot
    // (acceptable: only a few early pixels are affected).
    {
        int latchCycle = (activeWidth == 320) ? 112 : 109;
        if (!vsramSnapshotTaken && lineCycles >= latchCycle) {
            latchVscroll();
        }
    }

    if (pixelEnd < 0) {
        pixelEnd = 0;
    }
    if (pixelEnd > activeWidth) {
        pixelEnd = activeWidth;
    }
    if (pixelEnd <= renderedPixels) {
        return;
    }

    while (displayEnableEventCount > 0) {
        const VDPDisplayEnableEvent& event = displayEnableEvents[displayEnableEventReadIndex];
        if (event.scanline != scanline || event.pixel > pixelEnd) {
            break;
        }

        if (event.pixel > renderedPixels) {
            renderScanlineSegment(scanline, renderedPixels, event.pixel);
            renderedPixels = event.pixel;
        }

        displayEnabled = event.value;
        recordDisplayEnableApply(scanline, event.pixel, displayEnabled);
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DISP-APPLY] ln=%d px=%d val=%d reason=in-line-queue\n",
                         scanline,
                         event.pixel,
                         displayEnabled ? 1 : 0);
        }
        displayEnableEventReadIndex =
            (displayEnableEventReadIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
        displayEnableEventCount--;
    }

    if (pixelEnd > renderedPixels) {
        renderScanlineSegment(scanline, renderedPixels, pixelEnd);
        renderedPixels = pixelEnd;
    }
}

bool VDP::effectiveDisplayEnabled() const {
    bool enabled = displayEnabled;
    if (scanline >= activeHeight) {
        return enabled;
    }

    const int pixel = currentVisiblePixel();
    int index = displayEnableEventReadIndex;
    for (int remaining = displayEnableEventCount; remaining > 0; remaining--) {
        const VDPDisplayEnableEvent& event = displayEnableEvents[index];
        if (event.scanline > scanline) {
            break;
        }
        if (event.scanline == scanline && event.pixel > pixel) {
            break;
        }
        enabled = event.value;
        index = (index + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
    }
    return enabled;
}

void VDP::scheduleDisplayEnableChange(bool value) {
    displayEnabledLatch = value;

    if (scanline >= activeHeight) {
        displayEnabled = value;
        lineIsBlank = true;
        recordDisplayEnableApply(scanline, currentVisiblePixel(), displayEnabled);
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DISP-APPLY] ln=%d px=%d val=%d reason=vblank-immediate\n",
                         scanline,
                         currentVisiblePixel(),
                         displayEnabled ? 1 : 0);
        }
        return;
    }

    int applyScanline = scanline;
    int applyPixel = activeWidth;

    // Match the verified hardware display-enable visibility model. For Mega
    // Drive mode 5, writes in HBlank or before the viewport starts affect the
    // whole current line, while active-display writes blank or reveal pixels
    // from a viewport-relative X position onward. Formula One depends on this
    // seam landing later than our older cycle2hc-derived approximation.
    //
    // Reference: verified hardware timings for display status changes during
    // active display.
    static constexpr int kViewportStartMclk = 860;
    int mclk = lineCycles * 7;

    if (mclk <= kViewportStartMclk) {
        // HBlank / left border writes affect the whole current viewport line.
        applyPixel = 0;
    } else {
        const int dotDivisor = (activeWidth == 320) ? 8 : 10;
        int viewportPixel = ((mclk - kViewportStartMclk) / dotDivisor) + 16;
        if (viewportPixel < activeWidth) {
            // Round up to 2-pixel boundary for the segmented renderer.
            applyPixel = ((viewportPixel + 1) / 2) * 2;
            if (applyPixel <= renderedPixels) {
                applyPixel = ((renderedPixels + 2) / 2) * 2;
            }
            if (applyPixel >= activeWidth) {
                applyPixel = 0;
                applyScanline++;
                if (applyScanline >= getTotalScanlines()) {
                    applyScanline = 0;
                }
            }
        } else {
            // After the visible viewport, the current line is unaffected and
            // the new state becomes visible from the next line onward.
            applyPixel = 0;
            applyScanline++;
            if (applyScanline >= getTotalScanlines()) {
                applyScanline = 0;
            }
        }
    }

    if (displayEnableEventCount >= DISPLAY_ENABLE_EVENT_QUEUE_SIZE) {
        displayEnableEvents[(displayEnableEventWriteIndex + DISPLAY_ENABLE_EVENT_QUEUE_SIZE - 1)
                            % DISPLAY_ENABLE_EVENT_QUEUE_SIZE] = {value, applyScanline, applyPixel};
        return;
    }

    displayEnableEvents[displayEnableEventWriteIndex] = {value, applyScanline, applyPixel};
    displayEnableEventWriteIndex =
        (displayEnableEventWriteIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
    displayEnableEventCount++;
}

int VDP::fifoWaitCycles() const {
    if (fifoCount < FIFO_SIZE) return 0;
    // Find the next external slot after the current one
    const VDPSlotTable& table = currentSlotTable();
    int curSlot = currentSlotIndex < 0 ? 0 : currentSlotIndex;
    if (curSlot >= table.count) curSlot = table.count - 1;
    int searchFrom = curSlot + 1;
    if (searchFrom >= table.count) return 1;
    int nextExt = table.nextExternalSlot[searchFrom];
    if (nextExt >= table.count) return 1;
    // Use reverse lookup to find the exact M68K cycle where the VDP reaches
    // the next external slot, avoiding ±1 error from mclk rounding.
    for (int c = lineCycles + 1; c < VDP_MAX_M68K_CYCLES; c++) {
        if (table.slotAtM68kCycle[c] >= nextExt) {
            return c - lineCycles;
        }
    }
    return 1;
}

int VDP::dmaWaitCycles() const {
    // Cycles until the next external slot where a DMA transfer can occur.
    const VDPSlotTable& table = currentSlotTable();
    int curSlot = currentSlotIndex < 0 ? 0 : currentSlotIndex;
    if (curSlot >= table.count) curSlot = table.count - 1;
    int searchFrom = curSlot + 1;
    if (searchFrom >= table.count) return 1;
    int nextExt = table.nextExternalSlot[searchFrom];
    if (nextExt >= table.count) return 1;
    for (int c = lineCycles + 1; c < VDP_MAX_M68K_CYCLES; c++) {
        if (table.slotAtM68kCycle[c] >= nextExt) {
            return c - lineCycles;
        }
    }
    return 1;
}

int VDP::fifoCountAtCycleOffset(int offset) const {
    if (fifoCount == 0 || offset <= 0) return fifoCount;
    const VDPSlotTable& table = currentSlotTable();
    int effectiveCycle = lineCycles + offset;
    if (effectiveCycle >= VDP_MAX_M68K_CYCLES) effectiveCycle = VDP_MAX_M68K_CYCLES - 1;
    int targetSlot = table.slotAtM68kCycle[effectiveCycle];

    int first = (currentSlotIndex < 0 ? 0 : currentSlotIndex + 1);
    if (first >= table.count) return fifoCount;

    // Count external slots between current position and target
    int extSlotsInRange = 0;
    int s = table.nextExternalSlot[first];
    while (s <= targetSlot && s < table.count) {
        extSlotsInRange++;
        s = (s + 1 < table.count) ? table.nextExternalSlot[s + 1] : table.count;
    }
    // Conservative: assume VRAM writes (2 external slots per entry).
    // This keeps projected count higher, delaying empty flag rather than
    // reporting it prematurely.
    int drained = extSlotsInRange / 2;
    int projected = fifoCount - drained;
    return projected < 0 ? 0 : projected;
}

bool VDP::inHBlankPeriod() const {
    if (currentSlotIndex < 0) {
        return true; // Before first slot = start-of-line HBlank
    }
    // HBlank status timing does not disappear when display is disabled; only
    // fetch bandwidth changes. Use active timing table for status bit behavior.
    const VDPSlotTable& table = (activeWidth == 320) ? kH40Active : kH32Active;
    return currentSlotIndex < table.hblankEndSlot || currentSlotIndex >= table.hblankStartSlot;
}

bool VDP::inHBlankPeriod(int cycleOffset) const {
    // On real hardware the VDP runs concurrently with the 68K, so a status
    // register read reflects the VDP's position at the moment of the bus
    // access — not the stale position from before the instruction started.
    // Use lineCycles + offset to estimate the true VDP position at read time.
    // Reference: Nemesis (SpritesMind VDP Internals) — HBlank status is
    // evaluated per-pixel in the analog output stage.
    int effectiveCycle = lineCycles + cycleOffset;
    if (effectiveCycle >= VDP_MAX_M68K_CYCLES) {
        return true;  // Past end of line = early HBlank of next line
    }
    if (effectiveCycle < 0) {
        return true;
    }
    const VDPSlotTable& table = (activeWidth == 320) ? kH40Active : kH32Active;
    int slot = table.slotAtM68kCycle[effectiveCycle];
    return slot < table.hblankEndSlot || slot >= table.hblankStartSlot;
}

u32 VDP::cramToRGB(u16 cramValue) {
    // CRAM format: ---- BBB- GGG- RRR-
    const u32 r = expandChannel3To8((cramValue >> 1) & 0x07);
    const u32 g = expandChannel3To8((cramValue >> 5) & 0x07);
    const u32 b = expandChannel3To8((cramValue >> 9) & 0x07);

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

u32 VDP::cramToRGBShadow(u16 cramValue) {
    // Shadow = half brightness
    const u32 r = expandChannel3To8((cramValue >> 1) & 0x07) >> 1;
    const u32 g = expandChannel3To8((cramValue >> 5) & 0x07) >> 1;
    const u32 b = expandChannel3To8((cramValue >> 9) & 0x07) >> 1;

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

u32 VDP::cramToRGBHighlight(u16 cramValue) {
    // Highlight = half brightness + 128 (brighter)
    u32 r = (expandChannel3To8((cramValue >> 1) & 0x07) >> 1) + 128;
    u32 g = (expandChannel3To8((cramValue >> 5) & 0x07) >> 1) + 128;
    u32 b = (expandChannel3To8((cramValue >> 9) & 0x07) >> 1) + 128;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void VDP::updateCachedColor(int index) {
    cramColors[index] = cramToRGB(cram[index]);
    cramShadowColors[index] = cramToRGBShadow(cram[index]);
    cramHighlightColors[index] = cramToRGBHighlight(cram[index]);
}

void VDP::refreshCachedColors() {
    for (int i = 0; i < 64; i++) {
        updateCachedColor(i);
    }
}

bool VDP::enqueueFIFOEntry(u16 value, u16 writeAddress, u8 writeCode, bool byteWrite, bool lowByte) {
    if (fifoCount >= FIFO_SIZE) {
        return false;
    }

    fifo[fifoWriteIndex].value = value;
    fifo[fifoWriteIndex].address = writeAddress;
    fifo[fifoWriteIndex].code = writeCode;
    fifo[fifoWriteIndex].byteWrite = byteWrite;
    fifo[fifoWriteIndex].lowByte = lowByte;
    fifoWriteIndex = (fifoWriteIndex + 1) % FIFO_SIZE;
    fifoCount++;

    return true;
}

void VDP::applyFIFOEntry(const VDPFifoEntry& entry) {
    const int target = entry.code & 0x0F;

    flushCurrentLineToCurrentCycle();

    switch (target) {
        case 1: // VRAM
            if (entry.byteWrite) {
                if (entry.lowByte) {
                    vram[(entry.address ^ 1) & 0xFFFF] = static_cast<u8>(entry.value);
                } else {
                    vram[entry.address & 0xFFFF] = static_cast<u8>(entry.value);
                }
            } else if (entry.address & 1) {
                vram[(entry.address - 1) & 0xFFFF] = entry.value & 0xFF;
                vram[entry.address & 0xFFFF] = entry.value >> 8;
            } else {
                vram[entry.address & 0xFFFF] = entry.value >> 8;
                vram[(entry.address + 1) & 0xFFFF] = entry.value & 0xFF;
            }
            break;

        case 3: { // CRAM
            const int idx = (entry.address >> 1) & 0x3F;
            u16 cur = cram[idx];
            if (entry.byteWrite) {
                if (entry.lowByte) {
                    cur = (cur & 0xFF00) | static_cast<u8>(entry.value);
                } else {
                    cur = (cur & 0x00FF) | (static_cast<u16>(entry.value & 0xFF) << 8);
                }
            } else {
                cur = entry.value;
            }
            cram[idx] = cur & 0x0EEE;
            updateCachedColor(idx);
            // CRAM dots: record pixel artifact when write occurs during active display
            static const bool cramDotsEnabled = []() {
                const char* env = std::getenv("GENESIS_CRAM_DOTS");
                return env && std::atoi(env) != 0;
            }();
            if (cramDotsEnabled && scanline < activeHeight && cramDotCount < MAX_CRAM_DOTS) {
                int dotPixel = currentVisiblePixel();
                if (dotPixel > 0 && dotPixel < activeWidth) {
                    cramDots[cramDotCount++] = {dotPixel, cramToRGB(cur & 0x0EEE)};
                }
            }
            static const bool logHintTiming = []() {
                const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
                return env && std::atoi(env) != 0;
            }();
            if (logHintTiming && idx == (regs[0x07] & 0x3F)) {
                std::fprintf(stderr,
                             "[CRAM-BG] ln=%d cyc=%d px=%d idx=%02X val=%04X dispEn=%d\n",
                             scanline,
                             lineCycles,
                             currentVisiblePixel(),
                             idx,
                             cram[idx],
                             displayEnabled ? 1 : 0);
            }
            break;
        }

        case 5: { // VSRAM
            // The word write was already applied immediately in writeData()
            // for timing reasons; re-apply here to handle byte writes from
            // FIFO and to keep the canonical FIFO→apply path consistent.
            if ((entry.address >> 1) < 40) {
                const int idx = (entry.address >> 1) & 0x3F;
                u16 cur = vsram[idx];
                if (entry.byteWrite) {
                    if (entry.lowByte) {
                        cur = (cur & 0x0700) | static_cast<u8>(entry.value);
                    } else {
                        cur = (cur & 0x00FF) | ((static_cast<u16>(entry.value) & 0x07) << 8);
                    }
                } else {
                    cur = entry.value;
                }
                vsram[idx] = cur & 0x07FF;
                // Don't update vsramSnapshot — frozen once latched.
            }
            break;
        }

        default:
            break;
    }
}
