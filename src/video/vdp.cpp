// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"
#include "video/hvc_tables.h"
#include "memory/bus.h"
#include "debug_flags.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
inline u32 expandChannel3To8(int value) {
    return (value << 5) | (value << 2) | (value >> 1);
}

inline int defaultVblankIrqAssertCycle(int activeWidth) {
    // V-int is asserted at 788 MCLK in H40 and 770 MCLK in H32.
    // Floor division gives the M68K cycle at which the assertion occurs.
    const int mclk = (activeWidth == 320) ? 788 : 770;
    return mclk / 7;
}

inline int vblankIrqDelayCycles(int activeWidth) {
    const char* env = std::getenv("GENESIS_VINT_DELAY_CYCLES");
    if (!env) {
        return defaultVblankIrqAssertCycle(activeWidth);
    }
    int value = std::atoi(env);
    return value >= 0 ? value : defaultVblankIrqAssertCycle(activeWidth);
}

inline int hblankIrqAssertCycle(int defaultCycle) {
    const char* env = std::getenv("GENESIS_HINT_ASSERT_CYCLE");
    if (!env) {
        return defaultCycle;
    }
    int value = std::atoi(env);
    if (value < 0) {
        return defaultCycle;
    }
    if (value >= VDP_MAX_M68K_CYCLES) {
        return VDP_MAX_M68K_CYCLES - 1;
    }
    return value;
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

inline bool fifoTimingLogEnabled() {
    static int e = -1;
    if (e < 0) { const char* v = std::getenv("GENESIS_LOG_FIFO_TIMING"); e = (v && std::atoi(v)) ? 1 : 0; }
    return e != 0;
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

struct ScanlineEventRange {
    int first = -1;
    int last = -1;
    bool active = false;
};

inline ScanlineEventRange getScanlineEventRange() {
    static ScanlineEventRange range = []() {
        ScanlineEventRange r;
        const char* env = std::getenv("GENESIS_LOG_SCANLINE_EVENTS");
        if (env) {
            if (std::sscanf(env, "%d-%d", &r.first, &r.last) == 2) {
                r.active = true;
            } else if (std::sscanf(env, "%d", &r.first) == 1) {
                r.last = r.first;
                r.active = true;
            }
        }
        return r;
    }();
    return range;
}

inline bool scanlineEventLogActive(int scanline) {
    const auto& r = getScanlineEventRange();
    return r.active && scanline >= r.first && scanline <= r.last;
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

inline bool vdpPortTraceEnabled(int frame, int scanline) {
    static int enabled = -1;
    static int frameFirst = 0;
    static int frameLast = 0;
    static int lineFirst = 0;
    static int lineLast = 0;
    if (enabled < 0) {
        const char* env = std::getenv("GENESIS_LOG_VDP_PORT_TRACE");
        enabled = (env && env[0] != '0') ? 1 : 0;
        const char* ff = std::getenv("GENESIS_LOG_VDP_PORT_FRAME_FIRST");
        const char* fl = std::getenv("GENESIS_LOG_VDP_PORT_FRAME_LAST");
        const char* lf = std::getenv("GENESIS_LOG_VDP_PORT_LINE_FIRST");
        const char* ll = std::getenv("GENESIS_LOG_VDP_PORT_LINE_LAST");
        frameFirst = ff ? std::atoi(ff) : 0;
        frameLast = fl ? std::atoi(fl) : 0;
        lineFirst = lf ? std::atoi(lf) : 0;
        lineLast = ll ? std::atoi(ll) : 0;
    }
    if (!enabled) {
        return false;
    }
    if ((frameFirst || frameLast) &&
        (frame < frameFirst || frame > (frameLast ? frameLast : frameFirst))) {
        return false;
    }
    if ((lineFirst || lineLast) &&
        (scanline < lineFirst || scanline > (lineLast ? lineLast : lineFirst))) {
        return false;
    }
    return true;
}

inline bool hvcReadTraceEnabled(int frame, int scanline) {
    static int enabled = -1;
    static int frameFirst = 0;
    static int frameLast = 0;
    static int lineFirst = 0;
    static int lineLast = 0;
    if (enabled < 0) {
        const char* env = std::getenv("GENESIS_LOG_HVC_READ");
        enabled = (env && env[0] != '0') ? 1 : 0;
        const char* ff = std::getenv("GENESIS_LOG_HVC_FRAME_FIRST");
        const char* fl = std::getenv("GENESIS_LOG_HVC_FRAME_LAST");
        const char* lf = std::getenv("GENESIS_LOG_HVC_LINE_FIRST");
        const char* ll = std::getenv("GENESIS_LOG_HVC_LINE_LAST");
        frameFirst = ff ? std::atoi(ff) : 0;
        frameLast = fl ? std::atoi(fl) : 0;
        lineFirst = lf ? std::atoi(lf) : 0;
        lineLast = ll ? std::atoi(ll) : 0;
    }
    if (!enabled) {
        return false;
    }
    if ((frameFirst || frameLast) &&
        (frame < frameFirst || frame > (frameLast ? frameLast : frameFirst))) {
        return false;
    }
    if ((lineFirst || lineLast) &&
        (scanline < lineFirst || scanline > (lineLast ? lineLast : lineFirst))) {
        return false;
    }
    return true;
}

inline bool scrollLogEnabledForFrameLine(int frame, int scanline) {
    static int enabled = -1;
    static int frameFirst = 0;
    static int frameLast = 0;
    static int lineFirst = 0;
    static int lineLast = 0;
    if (enabled < 0) {
        const char* env = std::getenv("GENESIS_LOG_SCROLL");
        enabled = (env && env[0] != '0') ? 1 : 0;
        const char* ff = std::getenv("GENESIS_LOG_SCROLL_FRAME_FIRST");
        const char* fl = std::getenv("GENESIS_LOG_SCROLL_FRAME_LAST");
        const char* lf = std::getenv("GENESIS_LOG_SCROLL_LINE_FIRST");
        const char* ll = std::getenv("GENESIS_LOG_SCROLL_LINE_LAST");
        frameFirst = ff ? std::atoi(ff) : 0;
        frameLast = fl ? std::atoi(fl) : 0;
        lineFirst = lf ? std::atoi(lf) : 0;
        lineLast = ll ? std::atoi(ll) : 0;
    }
    if (!enabled) {
        return false;
    }
    if ((frameFirst || frameLast) &&
        (frame < frameFirst || frame > (frameLast ? frameLast : frameFirst))) {
        return false;
    }
    if ((lineFirst || lineLast) &&
        (scanline < lineFirst || scanline > (lineLast ? lineLast : lineFirst))) {
        return false;
    }
    return true;
}

inline const char* vdpTargetName(u8 code) {
    switch (code & 0x0F) {
        case 0: return "VRAM-R";
        case 1: return "VRAM-W";
        case 3: return "CRAM-W";
        case 4: return "VSRAM-R";
        case 5: return "VSRAM-W";
        case 8: return "CRAM-R";
        default: return "OTHER";
    }
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
    std::memset(vscrollLatch, 0, sizeof(vscrollLatch));
    std::memset(visibleLineVscroll, 0, sizeof(visibleLineVscroll));
    std::memset(boundaryVscrollLatch_, 0, sizeof(boundaryVscrollLatch_));
    boundaryVscrollLatchValid_ = false;
    std::memset(hscrollSnapshot, 0, sizeof(hscrollSnapshot));
    std::memset(columnVscroll, 0, sizeof(columnVscroll));
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
    cpuTimingBaseLineCycle_ = 0;
    currentSlotIndex = -1;
    hintAssertCycle_ = 0;
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
    const bool logPortTrace = vdpPortTraceEnabled(debugFrameNumber_, scanline);
    if (logPortTrace) {
        std::fprintf(stderr,
                     "[VDP-CTRL-IN] frame=%d ln=%d cyc=%d val=%04X pend=%d lat=%04X code=%02X(%s) addr=%04X\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     value,
                     controlPending ? 1 : 0,
                     controlLatch,
                     code,
                     vdpTargetName(code),
                     address);
    }

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
        // Register writes are not memory control commands. They cancel any
        // half-written command, but the active data-port target and address
        // continue unchanged so interrupt handlers can touch VDP registers
        // while mainline code streams data.
        int reg = (value >> 8) & 0x1F;
        if (reg < 24) {
            const u8 oldRegValue = regs[reg];
            flushCurrentLineToCurrentCycle();
            regs[reg] = value & 0xFF;
            decodeRegister(reg);
            if (logPortTrace) {
                std::fprintf(stderr,
                             "[VDP-REG-W] frame=%d ln=%d cyc=%d reg=%02X old=%02X new=%02X code=%02X(%s) addr=%04X\n",
                             debugFrameNumber_,
                             scanline,
                             lineCycles,
                             reg,
                             oldRegValue,
                             regs[reg],
                             code,
                             vdpTargetName(code),
                             address);
            }
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
        if (logPortTrace) {
            std::fprintf(stderr,
                         "[VDP-CTRL-1] frame=%d ln=%d cyc=%d val=%04X code=%02X(%s) addr=%04X\n",
                         debugFrameNumber_,
                         scanline,
                         lineCycles,
                         value,
                         code,
                         vdpTargetName(code),
                         address);
        }
        return;
    }

    controlPending = false;

    // Two-word command decode.
    // Address bits: A13..A0 in first word, A15..A14 in second word bits 1..0.
    address = (controlLatch & 0x3FFF) | ((value & 0x0003) << 14);

    // Code bits: CD1..CD0 in first word bits 15..14, CD5..CD2 in second word bits 7..4.
    code = static_cast<u8>(((controlLatch >> 14) & 0x03) | ((value >> 2) & 0x3C));
    if (logPortTrace) {
        std::fprintf(stderr,
                     "[VDP-CTRL-2] frame=%d ln=%d cyc=%d val=%04X latch=%04X code=%02X(%s) addr=%04X dma=%d\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     value,
                     controlLatch,
                     code,
                     vdpTargetName(code),
                     address,
                     ((code & 0x20) && dmaEnabled) ? 1 : 0);
    }

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
    // Reference: Nemesis VDP internals, confirmed by hardware testing.
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
    const bool logPortTrace = vdpPortTraceEnabled(debugFrameNumber_, scanline);

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
    if (logPortTrace) {
        const u32 a6 = bus ? bus->getM68KA6() : 0;
        const u32 pc = bus ? bus->getM68KPC() : 0;
        u16 a6m4 = 0;
        u16 a6m2 = 0;
        u16 a60 = 0;
        u16 a62 = 0;
        if (bus && (a6 & 0xFF0000) == 0xFF0000) {
            const u8* ram = bus->getRam();
            auto ramWord = [&](u32 addr) -> u16 {
                const u32 off = addr & 0xFFFF;
                return static_cast<u16>((ram[off] << 8) | ram[(off + 1) & 0xFFFF]);
            };
            a6m4 = ramWord((a6 - 4) & 0xFFFFFF);
            a6m2 = ramWord((a6 - 2) & 0xFFFFFF);
            a60 = ramWord(a6 & 0xFFFFFF);
            a62 = ramWord((a6 + 2) & 0xFFFFFF);
        }
        std::fprintf(stderr,
                     "[VDP-DATA-W] frame=%d ln=%d cyc=%d pc=%06X val=%04X code=%02X(%s) addr=%04X inc=%02X fifo=%d vs0=%03X vs1=%03X a6=%08X m[a6-4]=%04X m[a6-2]=%04X m[a6]=%04X m[a6+2]=%04X\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     pc & 0xFFFFFF,
                     value,
                     code,
                     vdpTargetName(code),
                     address,
                     regs[15],
                     fifoCount,
                     vsram[0] & 0x7FF,
                     vsram[1] & 0x7FF,
                     a6,
                     a6m4,
                     a6m2,
                     a60,
                     a62);
    }
    enqueueFIFOEntry(value, address, code, false, false);

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

    enqueueFIFOEntry(value, address, code, true, lowByte);

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
        if (hvcReadTraceEnabled(debugFrameNumber_, scanline)) {
            std::fprintf(stderr,
                         "[HVC-READ] frame=%d ln=%d cyc=%d mclk=%d latched=1 hv=%04X\n",
                         debugFrameNumber_,
                         scanline,
                         lineCycles,
                         lineCycles * 7,
                         hvLatch);
        }
        return hvLatch;
    }

    const u8 vcounter = (videoStandard_ == VideoStandard::PAL)
                            ? vCounterPal(scanline, activeHeight)
                            : vCounterNtsc(scanline);
    // Use per-master-clock H counter tables for cycle-accurate reads.
    // Per-master-clock precision fixes games that read the HV counter
    // mid-slot (Panorama Cotton, Skitchin, Lotus 2, Dashin Desperados).
    int mclk = lineCycles * 7;
    if (mclk >= 3420) mclk = 3419;
    if (mclk < 0) mclk = 0;
    u8 hcounterVal = (activeWidth == 320) ? cycle2hc40[mclk] : cycle2hc32[mclk];
    const u16 hv = (static_cast<u16>(vcounter) << 8) | hcounterVal;
    if (hvcReadTraceEnabled(debugFrameNumber_, scanline)) {
        std::fprintf(stderr,
                     "[HVC-READ] frame=%d ln=%d cyc=%d mclk=%d latched=0 hv=%04X hc=%02X vc=%02X\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     mclk,
                     hv,
                     hcounterVal,
                     vcounter);
    }
    return hv;
}

void VDP::setVideoStandard(VideoStandard standard) {
    videoStandard_ = standard;
}

int VDP::getTotalScanlines() const {
    return videoStandard_ == VideoStandard::PAL ? 313 : 262;
}

int VDP::getOutputYOffset() const {
    // PAL 224-line modes are displayed inside the 240-line PAL output
    // aperture. Internal timing still enters VBlank at activeHeight.
    return (videoStandard_ == VideoStandard::PAL && activeHeight == 224) ? 17 : 0;
}

int VDP::getOutputHeight() const {
    int height = activeHeight;
    if (videoStandard_ == VideoStandard::PAL && height == 224) {
        height = 240;
    }
    if (interlaceMode == 3) {
        height *= 2;
    }
    return height;
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
    scrollSampleLogged_ = false;
    renderedPixels = 0;
    nextColumnPixel = 8;  // First column boundary at pixel 8 (column 0 = pixels 0-7)
    columnTilesFetched = false;
    cramDotCount = 0;

    if (scanline == 0 && getOutputYOffset() > 0) {
        clearOutputFrame();
    }

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

    // Full-screen vscroll latch: H40 uses the boundary sample captured before
    // late-HINT carry-over drains. H32 samples before visible output, after
    // previous-line carry-over VSRAM writes but before same-line H-int code can
    // rewrite VSRAM for future lines.
    const bool h32FullScreenVscroll =
        activeWidth != 320 && vscrollMode == 0 && scanline < activeHeight;
    if (h32FullScreenVscroll) {
        commitCarryoverVscrollWritesForLineSample();
    }

    const bool useBoundaryVscroll =
        boundaryVscrollLatchValid_ && activeWidth == 320 &&
        vscrollMode == 0 && scanline < activeHeight;
    if (useBoundaryVscroll) {
        vscrollLatch[0] = boundaryVscrollLatch_[0];
        vscrollLatch[1] = boundaryVscrollLatch_[1];
    } else {
        vscrollLatch[0] = vsram[0];
        vscrollLatch[1] = vsram[1];
    }
    boundaryVscrollLatchValid_ = false;
    visibleLineVscroll[0] = vscrollLatch[0];
    visibleLineVscroll[1] = vscrollLatch[1];

    // H40 drains pre-line VSRAM after the line-wide sample so full-screen
    // rendering keeps the pre-HINT value, while later per-column / 2-cell
    // paths see updated vsram when they read it live.
    if (!h32FullScreenVscroll) {
        commitCarryoverVscrollWritesForLineSample();
    }
    if (vdpPortTraceEnabled(debugFrameNumber_, scanline)) {
        std::fprintf(stderr,
                     "[VDP-LINE-LATCH] frame=%d ln=%d cyc=%d vs0=%03X vs1=%03X vis0=%03X vis1=%03X lat0=%03X lat1=%03X hs0=%03X hs1=%03X fifo=%d\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     vsram[0] & 0x7FF,
                     vsram[1] & 0x7FF,
                     visibleLineVscroll[0] & 0x7FF,
                     visibleLineVscroll[1] & 0x7FF,
                     vscrollLatch[0] & 0x7FF,
                     vscrollLatch[1] & 0x7FF,
                     hscrollSnapshot[0] & 0x3FF,
                     hscrollSnapshot[1] & 0x3FF,
                     fifoCount);
    }

    // Keep the per-column buffer in sync with the current line-start VSRAM
    // state. Full-screen H40 rendering uses visibleLineVscroll; columnVscroll
    // remains the slot-sampled source for future/current per-column paths.
    for (int i = 0; i < 42; i++) {
        columnVscroll[i][0] = vsram[0];
        columnVscroll[i][1] = vsram[1];
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
    if (!lineIsBlank) {
        const char* env = std::getenv("GENESIS_DEBUG_USE_COLUMN_TILES");
        if (env && env[0] != '0') {
            fetchColumnTiles(scanline);
        }
    }
    const VDPSlotTable& activeTable = (activeWidth == 320) ? kH40Active : kH32Active;
    hcounter = activeTable.hcounterAtSlot[0];

    // H-int assert edge, decoupled from the HBlank status edge. Precomputed
    // in the slot table via reverse lookup, then optionally overridden for
    // diagnostics with GENESIS_HINT_ASSERT_CYCLE.
    hintAssertCycle_ = hblankIrqAssertCycle(activeTable.hintAssertM68kCycle);

    // H-int counter at line boundaries:
    // - VBlank: reload from reg[0x0A] every line.
    // - Active lines: decrement/assert at the H-int edge in clockM68K().
    // Non-active lines reload, active lines decrement, and assertion happens
    // only when the active-line counter reaches zero.
    if (scanline >= activeHeight) {
        hblankCounter = regs[0x0A];
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

void VDP::commitCarryoverVscrollWritesForLineSample() {
    if (scanline <= 0 || scanline > activeHeight) {
        return;
    }

    // Only full-screen vertical scroll uses the line-wide sample. In 2-cell
    // mode the renderer reads per-column/live values, so no line-start commit
    // is needed here.
    if (regs[0x0B] & 0x04) {
        return;
    }

    while (fifoCount > 0) {
        const VDPFifoEntry& head = fifo[fifoReadIndex];
        if ((head.code & 0x07) != 5) {
            break;
        }
        if (head.enqueueScanline == scanline) {
            break;
        }

        const VDPFifoEntry entry = head;
        fifoReadIndex = (fifoReadIndex + 1) % FIFO_SIZE;
        fifoCount--;
        applyFIFOEntry(entry);
    }
}

void VDP::logScrollSample() {
    if (scrollSampleLogged_ ||
        scanline >= activeHeight ||
        lineIsBlank ||
        !scrollLogEnabledForFrameLine(debugFrameNumber_, scanline)) {
        return;
    }

    scrollSampleLogged_ = true;
    std::fprintf(stderr,
                 "[SCROLL] ln=%d vsA=%d vsB=%d rawA=%d rawB=%d hsA=%d hsB=%d\n",
                 scanline,
                 vscrollLatch[0] & 0x3FF,
                 vscrollLatch[1] & 0x3FF,
                 vsram[0] & 0x3FF,
                 vsram[1] & 0x3FF,
                 hscrollSnapshot[0] & 0x3FF,
                 hscrollSnapshot[1] & 0x3FF);
}

void VDP::refreshVBlankIRQState() {
    if (scanline != activeHeight) {
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

int VDP::getVBlankIRQAssertCycle() const {
    return vblankIrqDelayCycles(activeWidth);
}

void VDP::beginScanline() {
    beginScanlineTimed();
}

void VDP::endScanline() {
    endScanlineTimed();

    captureBoundaryVscrollForNextLine();

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
        // FIFO write latency: entries enqueued on the current scanline
        // must wait FIFO_LATENCY_M68K cycles before they can drain.
        // Cross-line entries (from previous scanline) are always ready.
        const VDPFifoEntry& head = fifo[fifoReadIndex];
        if (head.enqueueScanline == scanline &&
            lineCycles < head.enqueueLineCycle + FIFO_LATENCY_M68K) {
            // Entry not ready yet — fall through to DMA
            if (dmaActive && dmaWordsRemaining > 0) {
                if (scanlineEventLogActive(scanline)) {
                    std::fprintf(stderr, "[EVT] ln=%d cyc=%d event=DMA-SLOT slot=%d dmaRem=%u\n",
                                 scanline, lineCycles, currentSlotIndex, dmaWordsRemaining);
                }
                processDMASlot();
            }
            return;
        }

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
        if (scanlineEventLogActive(scanline)) {
            const char* tgt = "???";
            switch (entry.code & 0x07) { case 1: tgt="VRAM"; break; case 3: tgt="CRAM"; break; case 5: tgt="VSRAM"; break; }
            std::fprintf(stderr, "[EVT] ln=%d cyc=%d event=FIFO-DRAIN slot=%d addr=%04X val=%04X target=%s fifoN=%d\n",
                         scanline, lineCycles, currentSlotIndex, entry.address, entry.value, tgt, fifoCount);
        }
        if (dmaActive && dmaWordsRemaining > 0 && fifoCount < FIFO_SIZE) {
            processDMASlot();
        }
        return;
    }

    if (dmaActive && dmaWordsRemaining > 0) {
        if (scanlineEventLogActive(scanline)) {
            std::fprintf(stderr, "[EVT] ln=%d cyc=%d event=DMA-SLOT slot=%d dmaRem=%u\n",
                         scanline, lineCycles, currentSlotIndex, dmaWordsRemaining);
        }
        processDMASlot();
    }
}

void VDP::syncToCpuTimingOffset(int partialCycles) {
    if (partialCycles <= 0) {
        return;
    }
    syncToLineCycle(cpuTimingBaseLineCycle_ + partialCycles);
}

void VDP::syncToLineCycle(int targetCycle) {
    if (targetCycle <= lineCycles) {
        return;
    }

    int lineLimit = lineCycleBudget;
    if (lineLimit <= 0) {
        lineLimit = VDP_MAX_M68K_CYCLES;
    }
    if (targetCycle > lineLimit) {
        targetCycle = lineLimit;
    }
    if (targetCycle <= lineCycles) {
        return;
    }

    clockM68K(targetCycle - lineCycles);
}

int VDP::nextCpuEventLineCycle() const {
    int nextCycle = lineCycleBudget;
    if (nextCycle <= 0) {
        nextCycle = VDP_MAX_M68K_CYCLES;
    }

    if ((fifoCount > 0 || (dmaActive && dmaWordsRemaining > 0))) {
        const VDPSlotTable& table = currentSlotTable();
        int curSlot = currentSlotIndex < 0 ? -1 : currentSlotIndex;
        int searchFrom = curSlot + 1;
        if (searchFrom < 0) searchFrom = 0;
        if (searchFrom < table.count) {
            int nextExt = table.nextExternalSlot[searchFrom];
            if (nextExt < table.count) {
                for (int c = lineCycles + 1; c < nextCycle; c++) {
                    if (table.slotAtM68kCycle[c] >= nextExt) {
                        nextCycle = c;
                        break;
                    }
                }
            }
        }
    }

    if (!hblankCounterDecremented_ && scanline < activeHeight &&
        hintAssertCycle_ > lineCycles && hintAssertCycle_ < nextCycle) {
        nextCycle = hintAssertCycle_;
    }

    if (vblankIRQArmed && scanline == activeHeight &&
        vblankIRQAssertCycle > lineCycles && vblankIRQAssertCycle < nextCycle) {
        nextCycle = vblankIRQAssertCycle;
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
                if (eventCycle > lineCycles && eventCycle < nextCycle) {
                    nextCycle = eventCycle;
                    break;
                }
            }
            index = (index + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
        }
    }

    return nextCycle;
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

        // Process display-enable events at their exact M68K cycle. This is
        // decoupled from rendering — clockM68K handles the state change,
        // flushCurrentLineToPixel just renders with the current displayEnabled.
        if (scanline < activeHeight && displayEnableEventCount > 0) {
            const VDPDisplayEnableEvent& event = displayEnableEvents[displayEnableEventReadIndex];
            if (event.scanline == scanline) {
                int eventCycle = m68kCycleForVisiblePixel(event.pixel);
                if (eventCycle > lineCycles && eventCycle < newLineCycles) {
                    // Clamp advance to event boundary
                    newLineCycles = eventCycle;
                }
                if (eventCycle > lineCycles && eventCycle <= newLineCycles) {
                    // We're at the event boundary — flush rendering with OLD
                    // displayEnabled, then apply the event.
                    flushCurrentLineToPixel(event.pixel);
                    displayEnabled = event.value;
                    recordDisplayEnableApply(scanline, event.pixel, displayEnabled);
                    displayEnableEventReadIndex =
                        (displayEnableEventReadIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
                    displayEnableEventCount--;
                }
            } else if (event.scanline > scanline) {
                // Event is for a future scanline — ignore
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
            if (scanlineEventLogActive(scanline)) {
                std::fprintf(stderr, "[EVT] ln=%d cyc=%d event=VINT-ASSERT\n", scanline, lineCycles);
            }
            megaTimingLog("VINT-ASSERT");
        }

        if (!hblankCounterDecremented_ && newLineCycles >= hintAssertCycle_ &&
            scanline < activeHeight) {
            hblankCounterDecremented_ = true;
            if (hblankCounter == 0) {
                hblankCounter = regs[0x0A];
                hblankIRQ = true;
                if (logHintTiming) {
                    std::fprintf(stderr,
                                 "[HINT] ln=%d cyc=%d newCyc=%d hintCyc=%d reg0A=%02X bgIdx=%02X dispEn=%d blank=%d dma=%d dmaMode=%d dmaRem=%u enabled=%d\n",
                                 scanline,
                                 lineCycles,
                                 newLineCycles,
                                 hintAssertCycle_,
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
                if (scanlineEventLogActive(scanline)) {
                    std::fprintf(stderr, "[EVT] ln=%d cyc=%d event=HINT-ASSERT hc=%d reg0A=%02X\n",
                                 scanline, lineCycles, hblankCounter, regs[0x0A]);
                }
            } else {
                hblankCounter--;
            }
        }

        const VDPSlotTable& table = currentSlotTable();
        int targetSlot = table.slotAtM68kCycle[newLineCycles < VDP_MAX_M68K_CYCLES ? newLineCycles : VDP_MAX_M68K_CYCLES - 1];

        // Find the VSRAM latch slot position for this line (if active).
        // The latch must fire IN SEQUENCE with FIFO processing — after
        // external slots before it drain, but before external slots after
        // it drain. This is critical: TG2's carry-over handler drains at
        // an external slot BEFORE the latch, while Cotton's handler drains
        // at an external slot AFTER the latch.
        int vscrollLatchSlot = -1;
        if (scanline < activeHeight && !lineIsBlank) {
            const VDPSlotTable& tbl = (activeWidth == 320) ? kH40Active : kH32Active;
            int fromSlot = (currentSlotIndex < 0) ? 0 : currentSlotIndex + 1;
            for (int s = fromSlot; s <= targetSlot && s < tbl.count; s++) {
                if (tbl.types[s] == SLOT_VSCROLL_LATCH) {
                    vscrollLatchSlot = s;
                    break;
                }
            }
        }

        // Slot-driven processing: when FIFO or DMA is active, iterate ALL
        // slots (not just external) so rendering, FIFO drains, VSRAM latch,
        // and column output happen in the correct hardware sequence.
        // When FIFO/DMA is idle, only process VSRAM latch and external slots
        // (fast path — VRAM can't change so batch rendering at end-of-line
        // produces the same result).
        // Only use slot-driven rendering when FIFO has entries (CPU writes to
        // VDP). DMA alone doesn't trigger per-column rendering because fill/copy
        // DMA modifies VRAM as a "background" operation — games expect the
        // visible output to reflect end-of-line VRAM state, not mid-DMA state.
        static const bool slotDriveDmaOnly = []() {
            const char* env = std::getenv("GENESIS_DEBUG_SLOT_DRIVE_DMA");
            return env && env[0] != '0';
        }();
        const bool needsSlotDriven =
            (fifoCount > 0) ||
            (slotDriveDmaOnly && dmaActive && dmaWordsRemaining > 0);
        // hcounter lookups always use the active timing table
        const VDPSlotTable& hcTable = (activeWidth == 320) ? kH40Active : kH32Active;
        const int scrollSampleSlot = (activeWidth == 320) ? 165 : 133;
        const bool crossesScrollSample =
            scanline < activeHeight && !lineIsBlank &&
            !scrollSampleLogged_ &&
            currentSlotIndex < scrollSampleSlot &&
            targetSlot >= scrollSampleSlot;

        if (needsSlotDriven && currentSlotIndex < targetSlot) {
            int first = currentSlotIndex + 1;
            if (first < 0) first = 0;

            // Use the CURRENT slot table (active or blank) for slot iteration.
            // During VBlank the blank table is used (all external, no PAT_B1).
            int s = (first < table.count) ? table.nextActionSlot[first] : table.count;
            for (; s <= targetSlot && s < table.count;
                 s = (s + 1 < table.count) ? table.nextActionSlot[s + 1] : table.count) {
                const VDPSlotInfo& si = table.info[s < table.count ? s : table.count - 1];

                // Update timing to this slot's position
                currentSlotIndex = s;
                lineCycles = firstM68kCycleForSlot(table, s);
                hcounter = hcTable.hcounterAtSlot[s < hcTable.count
                                                       ? s
                                                       : hcTable.count - 1];

                if (s >= scrollSampleSlot) {
                    logScrollSample();
                }

                switch (si.type) {
                    case SLOT_EXTERNAL:
                        processExternalSlot();
                        break;
                    case SLOT_VSCROLL_LATCH:
                        // H40 full-screen vscroll no longer reads vscrollLatch
                        // (the renderer consumes columnVscroll[col][layer]
                        // captured at each SLOT_NT_A). Only H32 still latches
                        // line-wide here.
                        if (scanline < activeHeight && !lineIsBlank &&
                            !(regs[0x0B] & 0x04) && activeWidth != 320) {
                            vscrollLatch[0] = vsram[0];
                            vscrollLatch[1] = vsram[1];
                        }
                        vscrollLatchSlot = -1;
                        break;
                    case SLOT_PAT_B1:
                        // Column output: render 8 pixels at this column's slot.
                        // Display-enable events are handled in clockM68K's outer
                        // loop (decoupled from rendering), so this is safe.
                        if (scanline < activeHeight && !lineIsBlank && si.column != 0xFF) {
                            int colPixelEnd = (si.column + 1) * 8;
                            if (colPixelEnd > activeWidth) colPixelEnd = activeWidth;
                            if (colPixelEnd > renderedPixels) {
                                flushCurrentLineToPixel(colPixelEnd);
                                if (nextColumnPixel <= colPixelEnd)
                                    nextColumnPixel = colPixelEnd + 8;
                            }
                        }
                        break;
                    case SLOT_NT_A:
                        // Preserve the per-column capture buffer for timing
                        // experiments and future 2-cell modeling. The active
                        // full-screen renderer uses the line-wide vscrollLatch.
                        if (scanline < activeHeight && !lineIsBlank && si.column != 0xFF) {
                            columnVscroll[si.column][0] = vsram[0];
                            columnVscroll[si.column][1] = vsram[1];
                        }
                        break;
                    case SLOT_REFRESH:
                    case SLOT_PAT_A0:
                    case SLOT_PAT_A1:
                    case SLOT_SPR_MAP:
                    case SLOT_PAT_B0:
                    case SLOT_HSCROLL:
                    case SLOT_SAT_SCAN:
                    case SLOT_SPR_PATTERN:
                    case SLOT_PATTERN:
                    default:
                        break;  // No action yet — future per-slot operations
                    case SLOT_NT_B:
                        if (scanline < activeHeight && !lineIsBlank) {
                            const char* env = std::getenv("GENESIS_DEBUG_USE_COLUMN_TILES");
                            if (env && env[0] != '0') {
                                fetchColumnTiles(scanline);
                            }
                        }
                        break;
                }
            }
        } else if (currentSlotIndex < targetSlot) {
            // Fast path: only process external slots and VSRAM latch
            int first = currentSlotIndex + 1;
            if (first < 0) first = 0;
            int s = (first < table.count) ? table.nextExternalSlot[first] : table.count;
            while (s <= targetSlot && s < table.count) {
                if (vscrollLatchSlot >= 0 && s > vscrollLatchSlot) {
                    // H32 only — H40 full-screen uses per-column columnVscroll.
                    if (!(regs[0x0B] & 0x04) && activeWidth != 320) {
                        vscrollLatch[0] = vsram[0];
                        vscrollLatch[1] = vsram[1];
                    }
                    vscrollLatchSlot = -1;
                }
                currentSlotIndex = s;
                lineCycles = firstM68kCycleForSlot(table, s);
                hcounter = hcTable.hcounterAtSlot[s < hcTable.count
                                                          ? s
                                                          : hcTable.count - 1];
                if (s >= scrollSampleSlot) {
                    logScrollSample();
                }
                processExternalSlot();
                if (fifoCount == 0 && (!dmaActive || dmaWordsRemaining == 0)) {
                    break;
                }
                s = (s + 1 < table.count) ? table.nextExternalSlot[s + 1] : table.count;
            }
        }

        if (crossesScrollSample) {
            logScrollSample();
        }

        // Fire latch if not yet consumed. H32 only — H40 full-screen uses
        // per-column columnVscroll captured at each SLOT_NT_A slot.
        if (vscrollLatchSlot >= 0) {
            if (!(regs[0x0B] & 0x04) && activeWidth != 320) {
                vscrollLatch[0] = vsram[0];
                vscrollLatch[1] = vsram[1];
            }
        }

        currentSlotIndex = targetSlot;
        lineCycles = newLineCycles;
        hcounter = hcTable.hcounterAtSlot[targetSlot < hcTable.count
                                                   ? targetSlot
                                                   : hcTable.count - 1];

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
    if (pixelEnd < 0) {
        pixelEnd = 0;
    }
    if (pixelEnd > activeWidth) {
        pixelEnd = activeWidth;
    }
    if (pixelEnd <= renderedPixels) {
        return;
    }

    // Display-enable events are now processed in clockM68K at their exact
    // M68K cycle, decoupled from rendering. flushCurrentLineToPixel just renders.
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
    static const bool logHintTiming = []() {
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    displayEnabledLatch = value;

    if (scanline >= activeHeight) {
        displayEnabled = value;
        lineIsBlank = true;
        recordDisplayEnableApply(scanline, currentVisiblePixel(), displayEnabled);
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
        // H32 display-off writes in the left-edge output window still suppress
        // the whole line. The segmented renderer may already have flushed this
        // small region, so the pixel-0 apply path below overwrites it.
        if (!value && activeWidth == 256 && viewportPixel <= 48) {
            applyPixel = 0;
        } else if (viewportPixel < activeWidth) {
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
            if (!value) {
                displayEnabled = false;
                lineIsBlank = true;
                recordDisplayEnableApply(scanline, 0, displayEnabled);
                renderScanlineSegment(scanline, 0, activeWidth);
                return;
            }
            // After the visible viewport, display-enable writes reveal pixels
            // from the next line onward.
            applyPixel = 0;
            applyScanline++;
            if (applyScanline >= getTotalScanlines()) {
                applyScanline = 0;
            }
        }
    }

    if (applyScanline == scanline && applyPixel == 0) {
        displayEnabled = value;
        lineIsBlank = (scanline >= activeHeight) || !displayEnabled;
        recordDisplayEnableApply(scanline, 0, displayEnabled);
        if (!displayEnabled && renderedPixels > 0) {
            renderScanlineSegment(scanline, 0, renderedPixels);
        }
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DISP-APPLY] ln=%d px=0 val=%d reason=current-line-pixel0\n",
                         scanline,
                         displayEnabled ? 1 : 0);
        }
        return;
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
    const VDPSlotTable& table = currentSlotTable();
    int curSlot = currentSlotIndex < 0 ? 0 : currentSlotIndex;
    if (curSlot >= table.count) curSlot = table.count - 1;
    int searchFrom = curSlot + 1;
    if (searchFrom >= table.count) return 1;

    // The FIFO head entry must have its latency expired before it can drain.
    const VDPFifoEntry& head = fifo[fifoReadIndex];
    int readyCycle = 0;
    if (head.enqueueScanline == scanline) {
        readyCycle = head.enqueueLineCycle + FIFO_LATENCY_M68K;
    }

    int nextExt = table.nextExternalSlot[searchFrom];
    while (nextExt < table.count) {
        for (int c = lineCycles + 1; c < VDP_MAX_M68K_CYCLES; c++) {
            if (table.slotAtM68kCycle[c] >= nextExt) {
                if (c >= readyCycle) {
                    return c - lineCycles;
                }
                // This external slot is too early — try the next one
                int nextSearch = nextExt + 1;
                nextExt = (nextSearch < table.count)
                    ? table.nextExternalSlot[nextSearch]
                    : table.count;
                break;
            }
        }
        if (nextExt >= table.count) break;
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

    // Count external slots where the FIFO head entry's latency has expired.
    int extSlotsInRange = 0;
    int s = table.nextExternalSlot[first];
    while (s <= targetSlot && s < table.count) {
        int slotCycle = firstM68kCycleForSlot(table, s);
        bool ready = true;
        if (fifoCount > 0) {
            const VDPFifoEntry& head = fifo[fifoReadIndex];
            if (head.enqueueScanline == scanline &&
                slotCycle < head.enqueueLineCycle + FIFO_LATENCY_M68K) {
                ready = false;
            }
        }
        if (ready) {
            extSlotsInRange++;
        }
        s = (s + 1 < table.count) ? table.nextExternalSlot[s + 1] : table.count;
    }
    // Conservative: assume VRAM writes (2 external slots per entry).
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

bool VDP::advanceFifoForCpuBoundaryWait() {
    if (fifoCount <= 0) {
        return false;
    }

    // If a CPU data-port write stalls at the scanline boundary, the scheduler
    // cannot advance into the next line from inside the bus callback. Consume
    // the FIFO slot phase needed to make room without dropping the blocked
    // write; the outer scanline loop accounts for the bus wait cycles.
    const int target = fifo[fifoReadIndex].code & 0x07;
    if (target == 1 && !vramWriteSecondSlot) {
        vramWriteSecondSlot = true;
        return true;
    }

    if (target == 1) {
        vramWriteSecondSlot = false;
    }

    const VDPFifoEntry entry = fifo[fifoReadIndex];
    fifoReadIndex = (fifoReadIndex + 1) % FIFO_SIZE;
    fifoCount--;
    applyFIFOEntry(entry);
    return true;
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
    fifo[fifoWriteIndex].enqueueScanline = scanline;
    fifo[fifoWriteIndex].enqueueLineCycle = lineCycles;
    fifoWriteIndex = (fifoWriteIndex + 1) % FIFO_SIZE;
    fifoCount++;

    if (vdpPortTraceEnabled(debugFrameNumber_, scanline)) {
        std::fprintf(stderr,
                     "[VDP-FIFO-ENQ] frame=%d ln=%d cyc=%d addr=%04X code=%02X(%s) val=%04X byte=%d low=%d fifo=%d\n",
                     debugFrameNumber_,
                     scanline,
                     lineCycles,
                     writeAddress,
                     writeCode,
                     vdpTargetName(writeCode),
                     value,
                     byteWrite ? 1 : 0,
                     lowByte ? 1 : 0,
                     fifoCount);
    }

    if (fifoTimingLogEnabled()) {
        std::fprintf(stderr, "[FIFO-ENQ] ln=%d cyc=%d slot=%d addr=%04X code=%02X val=%04X fifoN=%d byte=%d\n",
                     scanline, lineCycles, currentSlotIndex,
                     writeAddress, writeCode, value, fifoCount, byteWrite ? 1 : 0);
    }

    return true;
}

void VDP::applyFIFOEntry(const VDPFifoEntry& entry) {
    const int target = entry.code & 0x0F;

    flushCurrentLineToCurrentCycle();

    if (fifoTimingLogEnabled()) {
        const char* tname = "???";
        switch (target) { case 1: tname = "VRAM"; break; case 3: tname = "CRAM"; break; case 5: tname = "VSRAM"; break; }
        std::fprintf(stderr, "[FIFO-APPLY] ln=%d cyc=%d slot=%d addr=%04X val=%04X target=%s px=%d\n",
                     scanline, lineCycles, currentSlotIndex,
                     entry.address, entry.value, tname, currentVisiblePixel());
    }

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
                const u16 old = cur;
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
                if (vdpPortTraceEnabled(debugFrameNumber_, scanline)) {
                    std::fprintf(stderr,
                                 "[VDP-VSRAM-APPLY] frame=%d ln=%d cyc=%d idx=%d old=%03X new=%03X entry=%04X addr=%04X code=%02X px=%d rend=%d\n",
                                 debugFrameNumber_,
                                 scanline,
                                 lineCycles,
                                 idx,
                                 old & 0x7FF,
                                 vsram[idx] & 0x7FF,
                                 entry.value,
                                 entry.address,
                                 entry.code,
                                 currentVisiblePixel(),
                                 renderedPixels);
                }
                if (fifoTimingLogEnabled()) {
                    std::fprintf(stderr, "[VSRAM-SET] ln=%d cyc=%d idx=%d val=%03X px=%d renderedPx=%d\n",
                                 scanline, lineCycles, idx,
                                 vsram[idx] & 0x7FF,
                                 currentVisiblePixel(), renderedPixels);
                }

                // H40 full-screen vscroll samples VSRAM around the line-change
                // phase, not at our scanline cycle-0 bookkeeping point. In our
                // rotated H40 slot table, writes that drain in
                // the early external windows before slot 65 are still before
                // the hardware-equivalent visible sample. Update the line-wide
                // sample and discard any premature segmented output so the line
                // is rendered from the coherent post-drain pair.
                constexpr int kH40FullScreenVscrollSampleWindowEndSlot = 66;
                if (scanline < activeHeight && !lineIsBlank &&
                    !(regs[0x0B] & 0x04) && activeWidth == 320 &&
                    currentSlotIndex < kH40FullScreenVscrollSampleWindowEndSlot &&
                    entry.enqueueScanline != scanline) {
                    const u16 oldVsA = vscrollLatch[0];
                    const u16 oldVsB = vscrollLatch[1];
                    vscrollLatch[0] = vsram[0];
                    vscrollLatch[1] = vsram[1];
                    visibleLineVscroll[0] = vscrollLatch[0];
                    visibleLineVscroll[1] = vscrollLatch[1];
                    if ((vscrollLatch[0] != oldVsA ||
                         vscrollLatch[1] != oldVsB) &&
                        renderedPixels > 0) {
                        renderedPixels = 0;
                        nextColumnPixel = 0;
                    }
                }

                // H32 full-screen scroll uses a provisional per-line sample
                // until the early-line latch window has passed. The H32
                // full-plane path samples both VSRAM words together; Top Gear
                // 2 is timing-sensitive to the adjacent external slot.
                // Keep this internal invalidation window through our second
                // early H32 external drain (slot 40), so the segmented
                // renderer never freezes an impossible half-updated pair
                // (new Plane A with stale Plane B).
                constexpr int kH32FullScreenVscrollLatchWindowEndSlot = 41;
                if (scanline < activeHeight && !lineIsBlank &&
                    !(regs[0x0B] & 0x04) && activeWidth != 320 &&
                    currentSlotIndex < kH32FullScreenVscrollLatchWindowEndSlot &&
                    entry.enqueueScanline != scanline) {
                    const u16 oldVsA = vscrollLatch[0];
                    const u16 oldVsB = vscrollLatch[1];
                    vscrollLatch[0] = vsram[0];
                    vscrollLatch[1] = vsram[1];
                    if ((vscrollLatch[0] != oldVsA ||
                         vscrollLatch[1] != oldVsB) &&
                        renderedPixels > 0) {
                        renderedPixels = 0;
                        nextColumnPixel = 0;
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}
