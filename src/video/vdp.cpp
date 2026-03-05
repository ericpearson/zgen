// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"
#include "memory/bus.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
inline u32 expandChannel3To8(int value) {
    return (value << 5) | (value << 2) | (value >> 1);
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
}

VDP::VDP() : bus(nullptr), videoStandard_(VideoStandard::NTSC) {
    reset();
}

void VDP::reset() {
    const VideoStandard preservedStandard = videoStandard_;

    std::memset(vram, 0, sizeof(vram));
    std::memset(cram, 0, sizeof(cram));
    std::memset(vsram, 0, sizeof(vsram));
    std::memset(regs, 0, sizeof(regs));
    std::memset(framebuffer, 0, sizeof(framebuffer));
    std::memset(fifo, 0, sizeof(fifo));

    controlPending = false;
    controlLatch = 0;
    code = 0;
    address = 0;

    dmaActive = false;
    dmaMode = 0;
    dmaBytesRemaining = 0;
    dmaWordsRemaining = 0;
    dmaFillPending = false;
    dmaFillValue = 0;
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
    hblankIRQ = false;
    hblankCounter = 0;

    spriteCollision = false;
    spriteOverflow = false;
    spriteLineCount[0] = 0;
    spriteLineCount[1] = 0;
    spriteLineValid[0] = false;
    spriteLineValid[1] = false;

    hvLatch = 0;
    hvLatched = false;

    // Common power-on increment value used by most software.
    regs[0x0F] = 2;

    // Defaults for decoded state.
    activeWidth = 320;
    activeHeight = 224;
    displayEnabled = false;
    displayEnabledLatch = false;
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
    hblankCounter = regs[0x0A];
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
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    // Register write (single word): 10RR RRRR DDDD DDDD
    // Always recognized regardless of controlPending state (real hardware behavior).
    if ((value & 0xC000) == 0x8000) {
        controlPending = false;
        // Register writes do NOT update the code register on real hardware.
        // Since bits 15:14 are always 10 for register writes, updating CD1:CD0
        // would set them to 2, corrupting the write target for subsequent data
        // port writes (e.g., turning VRAM write into an invalid target).
        int reg = (value >> 8) & 0x1F;
        if (reg < 24) {
            const u8 oldRegValue = regs[reg];
            flushCurrentLineToCurrentCycle();
            regs[reg] = value & 0xFF;
            decodeRegister(reg);
            if (logHintTiming && (reg == 0x01 || reg == 0x07)) {
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

    // FIFO status
    if (fifoCount == 0) {
        status |= 0x0200;  // FIFO empty
    }
    if (fifoCount >= FIFO_SIZE) {
        status |= 0x0100;  // FIFO full
    }

    if (vblankIRQ) {
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

    // Cycle-accurate HBlank status bit.  When partialCycles > 0 the
    // caller is providing the number of M68K cycles consumed so far within
    // the current instruction, giving a more accurate VDP position for the
    // HBlank check (the VDP runs concurrently on real hardware).
    if (partialCycles > 0 ? inHBlankPeriod(partialCycles) : inHBlankPeriod()) {
        status |= 0x0004;
    }

    if (dmaActive) {
        status |= 0x0002;
    }

    // Status reads clear only sprite collision/overflow latches.
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
        dmaFillValue = static_cast<u8>(value >> 8);
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
        dmaAddress = address;  // Fill begins from the post-increment address
        dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
        if (dmaWordsRemaining == 0) dmaWordsRemaining = 0x10000;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMAFILL-START] ln=%d cyc=%d addr=%04X len=%u inc=%02X fill=%02X code=%02X\n",
                         scanline,
                         lineCycles,
                         dmaAddress,
                         dmaWordsRemaining,
                         regs[15],
                         dmaFillValue,
                         code);
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
        dmaFillValue = value;
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
        dmaAddress = address;
        dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
        if (dmaWordsRemaining == 0) dmaWordsRemaining = 0x10000;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMAFILL-START] ln=%d cyc=%d addr=%04X len=%u inc=%02X fill=%02X code=%02X\n",
                         scanline,
                         lineCycles,
                         dmaAddress,
                         dmaWordsRemaining,
                         regs[15],
                         dmaFillValue,
                         code);
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
    u8 hcounterVal = static_cast<u8>(hcounter & 0xFF);
    return (static_cast<u16>(vcounter) << 8) | hcounterVal;
}

void VDP::setVideoStandard(VideoStandard standard) {
    videoStandard_ = standard;
}

int VDP::getTotalScanlines() const {
    return videoStandard_ == VideoStandard::PAL ? 313 : 262;
}

void VDP::decodeRegister(int reg) {
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
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    lineCycles = 0;
    currentSlotIndex = -1;  // No slot processed yet; first clockM68K processes from slot 0
    hblankCounterDecremented_ = false;
    renderedPixels = 0;
    spriteLineCount[0] = 0;
    spriteLineCount[1] = 0;
    spriteLineValid[0] = false;
    spriteLineValid[1] = false;

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

    // When display is disabled (reg 1 bit 6 clear), the VDP does not perform
    // pattern fetches, freeing those slots for FIFO/DMA — same bandwidth as
    // VBlank. Games like Sonic 2 2P rely on this for DMA during the divider.
    lineIsBlank = (scanline >= activeHeight) || !displayEnabled;
    hcounter = currentSlotTable().hcounterAtSlot[0];

    // Compute the M68K cycle at which HBlank starts this line (for H-int timing).
    // Use the active slot table since HBlank timing is fixed regardless of display-enable.
    const VDPSlotTable& activeTable = (activeWidth == 320) ? kH40Active : kH32Active;
    hblankStartCycle_ = activeTable.mclkOffset[activeTable.hblankStartSlot] / 7;

    if (scanline >= activeHeight) {
        // VBlank lines: reload counter every line
        hblankCounter = regs[0x0A];
    }
    // Active-line H-counter is decremented at HBlank start inside clockM68K()

    if (scanline == activeHeight && vblankEnabled) {
        vblankIRQ = true;
    }
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
    if (activeWidth == 320) {
        return lineIsBlank ? kH40Blank : kH40Active;
    }
    return lineIsBlank ? kH32Blank : kH32Active;
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

void VDP::clockM68K(int cycles) {
    static const bool logHintTiming = []() {
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

    int newLineCycles = lineCycles + cycles;
    if (newLineCycles > lineLimit) {
        newLineCycles = lineLimit;
    }

    // H-int counter fires at HBlank start (hardware-accurate timing)
    if (!hblankCounterDecremented_ && newLineCycles >= hblankStartCycle_ && scanline < activeHeight) {
        hblankCounterDecremented_ = true;
        if (hblankCounter == 0) {
            hblankCounter = regs[0x0A];
            if (hblankEnabled) {
                hblankIRQ = true;
                if (logHintTiming) {
                    std::fprintf(stderr,
                                 "[HINT] ln=%d cyc=%d newCyc=%d hbStart=%d reg0A=%02X bgIdx=%02X dispEn=%d blank=%d dma=%d dmaMode=%d dmaRem=%u\n",
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
                                 dmaWordsRemaining);
                }
            }
        } else {
            hblankCounter--;
        }
    }

    const VDPSlotTable& table = currentSlotTable();
    int targetSlot = table.slotAtM68kCycle[newLineCycles < VDP_MAX_M68K_CYCLES ? newLineCycles : VDP_MAX_M68K_CYCLES - 1];

    // Process external slots we're crossing — each slot exactly once.
    // currentSlotIndex is the last slot we've reached (-1 at line start).
    // New slots to process: (currentSlotIndex + 1) .. targetSlot.
    if ((fifoCount > 0 || (dmaActive && dmaWordsRemaining > 0)) && currentSlotIndex < targetSlot) {
        // Jump directly to external slots using the precomputed lookup
        int first = currentSlotIndex + 1;
        if (first < 0) first = 0;
        int s = (first < table.count) ? table.nextExternalSlot[first] : table.count;
        while (s <= targetSlot && s < table.count) {
            currentSlotIndex = s;
            processExternalSlot();
            if (fifoCount == 0 && (!dmaActive || dmaWordsRemaining == 0)) {
                break;
            }
            s = (s + 1 < table.count) ? table.nextExternalSlot[s + 1] : table.count;
        }
    }

    currentSlotIndex = targetSlot;
    lineCycles = newLineCycles;
    // H-counter timing is based on the display mode's fixed scan timing,
    // not on whether pattern fetches are suppressed by display disable.
    const VDPSlotTable& activeTimingTable = (activeWidth == 320) ? kH40Active : kH32Active;
    hcounter = activeTimingTable.hcounterAtSlot[targetSlot < activeTimingTable.count
                                                    ? targetSlot
                                                    : activeTimingTable.count - 1];
}

int VDP::currentVisiblePixel() const {
    if (scanline >= activeHeight) {
        return activeWidth;
    }
    if (currentSlotIndex < 0) {
        return 0;
    }
    // Visible pixel timing follows the active scan structure regardless of
    // display-enable state. Blank-table slots affect bandwidth, not where
    // the left/right blanking regions are on the scanline.
    const VDPSlotTable& table = (activeWidth == 320) ? kH40Active : kH32Active;
    int slot = currentSlotIndex;
    if (slot >= table.count) slot = table.count - 1;
    // Early HBlank (before active display starts): no pixels rendered yet.
    // Late HBlank (after active display ends): all pixels rendered.
    // pixelAtSlot uses activeWidth for both blanking regions, but the
    // rendering frontier semantics differ: early = 0, late = activeWidth.
    if (slot < table.hblankEndSlot) {
        return 0;
    }
    return table.pixelAtSlot[slot];
}

int VDP::nextVisibleSlotBoundaryPixel() const {
    if (scanline >= activeHeight) {
        return activeWidth;
    }
    // Use active timing for pixel-boundary computations; blank table is only
    // for bandwidth scheduling.
    const VDPSlotTable& table = (activeWidth == 320) ? kH40Active : kH32Active;
    int slot = currentSlotIndex + 1;
    if (slot < 0) slot = 0;
    if (slot >= table.count) return activeWidth;
    // Early HBlank: next visible boundary is pixel 0 (active display hasn't started)
    if (slot < table.hblankEndSlot) {
        return 0;
    }
    int pixel = table.pixelAtSlot[slot];
    // Round up to next 2-pixel boundary for segment alignment
    if (pixel < activeWidth) {
        pixel = ((pixel + 1) / 2) * 2;
    }
    return pixel;
}

void VDP::flushCurrentLineToCurrentCycle() {
    flushCurrentLineToPixel(currentVisiblePixel());
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

    if (inHBlankPeriod()) {
        const VDPSlotTable& activeTimingTable = (activeWidth == 320) ? kH40Active : kH32Active;
        const bool earlyHBlank = (currentSlotIndex < 0) || (currentSlotIndex < activeTimingTable.hblankEndSlot);
        if (!earlyHBlank) {
            // Late-HBlank writes take effect on the next line start.
            int nextLine = scanline + 1;
            if (nextLine >= getTotalScanlines()) {
                nextLine = 0;
            }
            if (displayEnableEventCount >= DISPLAY_ENABLE_EVENT_QUEUE_SIZE) {
                displayEnableEvents[(displayEnableEventWriteIndex + DISPLAY_ENABLE_EVENT_QUEUE_SIZE - 1)
                                    % DISPLAY_ENABLE_EVENT_QUEUE_SIZE] = {value, nextLine, 0};
            } else {
                displayEnableEvents[displayEnableEventWriteIndex] = {value, nextLine, 0};
                displayEnableEventWriteIndex =
                    (displayEnableEventWriteIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
                displayEnableEventCount++;
            }
            static const bool logHintTiming = []() {
                const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
                return env && std::atoi(env) != 0;
            }();
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DISP-QUEUE] ln=%d px=%d val=%d applyLn=%d applyPx=0 reason=hblank-late\n",
                             scanline,
                             currentVisiblePixel(),
                             value ? 1 : 0,
                             nextLine);
            }
            return;
        }

        displayEnabled = value;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DISP-APPLY] ln=%d px=%d val=%d reason=hblank-immediate\n",
                         scanline,
                         currentVisiblePixel(),
                         displayEnabled ? 1 : 0);
        }
        // Writes during early HBlank (before active display starts) should
        // affect whether this line renders as blank or active.
        if (currentSlotIndex < activeTimingTable.hblankEndSlot) {
            lineIsBlank = !displayEnabled;
        }
        return;
    }

    // During active display, defer display-enable changes to line end. This
    // avoids sub-line seam notches from writes that land a few pixels before
    // HBlank while still allowing early-HBlank writes to affect the line.
    const int applyPixel = activeWidth;
    if (displayEnableEventCount >= DISPLAY_ENABLE_EVENT_QUEUE_SIZE) {
        displayEnableEvents[(displayEnableEventWriteIndex + DISPLAY_ENABLE_EVENT_QUEUE_SIZE - 1)
                            % DISPLAY_ENABLE_EVENT_QUEUE_SIZE] = {value, scanline, applyPixel};
        return;
    }

    displayEnableEvents[displayEnableEventWriteIndex] = {value, scanline, applyPixel};
    displayEnableEventWriteIndex =
        (displayEnableEventWriteIndex + 1) % DISPLAY_ENABLE_EVENT_QUEUE_SIZE;
    displayEnableEventCount++;
}

int VDP::fifoWaitCycles() const {
    if (fifoCount < FIFO_SIZE) return 0;
    // Compute M68K cycles to the next external slot from the slot table
    const VDPSlotTable& table = currentSlotTable();
    int curSlot = currentSlotIndex < 0 ? 0 : currentSlotIndex;
    if (curSlot >= table.count) curSlot = table.count - 1;
    // Look for the next external slot AFTER the current one (since current is already processed)
    int searchFrom = curSlot + 1;
    if (searchFrom >= table.count) return 1;
    int nextExt = table.nextExternalSlot[searchFrom];
    if (nextExt >= table.count) return 1;
    // Convert slot distance to approximate M68K cycles
    int mclkNow = table.mclkOffset[curSlot];
    int mclkNext = table.mclkOffset[nextExt];
    int delta = mclkNext - mclkNow;
    if (delta <= 0) delta = 7;
    return (delta + 6) / 7; // Convert master clocks to M68K cycles (round up)
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
            }
            break;
        }

        default:
            break;
    }
}
