// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "video/vdp.h"
#include "memory/bus.h"
#include "debug_flags.h"
#include <cstdlib>
#include <cstdio>
void VDP::executeDMA() {
    if (!dmaEnabled) {
        return;
    }

    static const bool logHintTiming = []() {
        if (!g_debugMode) return false;
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();

    dmaMode = (regs[23] >> 6) & 0x03;

    // Starting any new DMA cancels a pending fill that hasn't been triggered
    // yet.  On real hardware the DMA unit only tracks one operation at a time.
    if (dmaMode != 2) {
        dmaFillPending = false;
    }

    switch (dmaMode) {
        case 0:
        case 1:
            dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
            if (dmaWordsRemaining == 0) {
                dmaWordsRemaining = 0x10000;
            }
            dmaActive = dmaWordsRemaining > 0;
            dmaVramSecondSlot = false;
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DMA-START] ln=%d cyc=%d mode=%d len=%u addr=%04X code=%02X src=%02X%02X%02X\n",
                             scanline,
                             lineCycles,
                             dmaMode,
                             dmaWordsRemaining,
                             address,
                             code,
                             regs[23] & 0x7F,
                             regs[22],
                             regs[21]);
            }
            break;
        case 2:
            // Fill is started by the next data-port write.
            dmaFillPending = true;
            dmaActive = false;
            dmaWordsRemaining = 0;
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DMA-START] ln=%d cyc=%d mode=2 pending-fill addr=%04X code=%02X\n",
                             scanline,
                             lineCycles,
                             address,
                             code);
            }
            break;
        case 3:
            // Bandwidth-limited VRAM copy (doesn't freeze 68K)
            dmaWordsRemaining = (static_cast<u32>(regs[20]) << 8) | regs[19];
            if (dmaWordsRemaining == 0) dmaWordsRemaining = 0x10000;
            dmaActive = true;
            dmaAddress = address;  // Capture separate DMA destination counter
            dmaCopySecondSlot = false;
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DMA-START] ln=%d cyc=%d mode=3 len=%u src=%02X%02X dst=%04X code=%02X\n",
                             scanline,
                             lineCycles,
                             dmaWordsRemaining,
                             regs[22],
                             regs[21],
                             dmaAddress,
                             code);
            }
            break;
    }
}

void VDP::dma68kToVRAM() {
    if (!bus || dmaWordsRemaining == 0) {
        dmaActive = false;
        regs[19] = 0;
        regs[20] = 0;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMA-END] ln=%d cyc=%d mode=%d addr=%04X\n",
                         scanline,
                         lineCycles,
                         dmaMode,
                         address);
        }
        if (cachedControlWritePending) {
            u16 cached = cachedControlWriteValue;
            cachedControlWritePending = false;
            writeControl(cached);
        }
        return;
    }

    // Source is a 24-bit, word-addressed 68K address.
    u32 srcAddr = ((static_cast<u32>(regs[23] & 0x7F) << 17) |
                   (static_cast<u32>(regs[22]) << 9) |
                   (static_cast<u32>(regs[21]) << 1)) & 0xFFFFFF;

    int target = code & 0x07;
    u16 data = bus->read16(srcAddr);
    if (target == 1 || target == 3 || target == 5) {
        flushCurrentLineToCurrentCycle();
    }

    switch (target) {
        case 1: // VRAM
            if (address & 1) {
                vram[(address - 1) & 0xFFFF] = data & 0xFF;
                vram[address & 0xFFFF] = data >> 8;
            } else {
                vram[address & 0xFFFF] = data >> 8;
                vram[(address + 1) & 0xFFFF] = data & 0xFF;
            }
            break;

        case 3: // CRAM
            cram[(address >> 1) & 0x3F] = data & 0x0EEE;
            updateCachedColor((address >> 1) & 0x3F);
            break;

        case 5: // VSRAM
            if ((address >> 1) < 40) {
                vsram[(address >> 1) & 0x3F] = data & 0x07FF;
            }
            break;

        default:
            break;
    }

    srcAddr = (srcAddr + 2) & 0xFFFFFF;
    address = static_cast<u16>(address + regs[15]);
    u32 srcWordAddr = (srcAddr >> 1) & 0x7FFFFF;
    regs[21] = srcWordAddr & 0xFF;
    regs[22] = (srcWordAddr >> 8) & 0xFF;
    regs[23] = (regs[23] & 0xC0) | ((srcWordAddr >> 16) & 0x7F);

    u16 lengthCounter = (static_cast<u16>(regs[20]) << 8) | regs[19];
    lengthCounter = static_cast<u16>(lengthCounter - 1);
    regs[19] = lengthCounter & 0xFF;
    regs[20] = (lengthCounter >> 8) & 0xFF;

    dmaWordsRemaining--;
    if (dmaWordsRemaining == 0) {
        dmaActive = false;
        regs[19] = 0;
        regs[20] = 0;
        static const bool logHintTiming = []() {
            const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
            return env && std::atoi(env) != 0;
        }();
        if (logHintTiming) {
            std::fprintf(stderr,
                         "[DMA-END] ln=%d cyc=%d mode=%d addr=%04X\n",
                         scanline,
                         lineCycles,
                         dmaMode,
                         address);
        }
        if (cachedControlWritePending) {
            u16 cached = cachedControlWriteValue;
            cachedControlWritePending = false;
            writeControl(cached);
        }
    }
}

void VDP::processDMASlot() {
    if (!dmaActive || dmaWordsRemaining == 0) return;

    if (dmaMode <= 1) {
        // 68K→VDP: VRAM target takes 2 slots per word, CRAM/VSRAM takes 1
        int target = code & 0x07;
        if (target == 1) {
            if (!dmaVramSecondSlot) {
                dmaVramSecondSlot = true;
                return;
            }
            dmaVramSecondSlot = false;
        }
        dma68kToVRAM();
    } else if (dmaMode == 2) {
        // Fill: 1 external slot per iteration.
        // Uses dmaAddress (not address) so control port writes during fill
        // don't corrupt the destination — real hardware uses an internal counter.
        // Uses latched dmaFillCode (not live code) — 68K keeps running and may
        // change code mid-fill via control port writes.
        int target = dmaFillCode & 0x07;
        flushCurrentLineToCurrentCycle();
        switch (target) {
            case 1: // VRAM: writes high byte to addr ^ 1
                vram[(dmaAddress ^ 1) & 0xFFFF] = static_cast<u8>(dmaFillValue >> 8);
                break;
            case 3: // CRAM: writes full word
                cram[(dmaAddress >> 1) & 0x3F] = dmaFillValue & 0x0EEE;
                updateCachedColor((dmaAddress >> 1) & 0x3F);
                break;
            case 5: // VSRAM: writes full word
                if ((dmaAddress >> 1) < 40) {
                    vsram[(dmaAddress >> 1) & 0x3F] = dmaFillValue & 0x07FF;
                }
                break;
            default:
                break;
        }
        dmaAddress = static_cast<u16>(dmaAddress + regs[15]);

        u16 lengthCounter = (static_cast<u16>(regs[20]) << 8) | regs[19];
        lengthCounter = static_cast<u16>(lengthCounter - 1);
        regs[19] = lengthCounter & 0xFF;
        regs[20] = (lengthCounter >> 8) & 0xFF;

        dmaWordsRemaining--;
        if (dmaWordsRemaining == 0) {
            dmaActive = false;
            address = dmaAddress;
            regs[19] = 0;
            regs[20] = 0;
            static const bool logHintTiming = []() {
                const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
                return env && std::atoi(env) != 0;
            }();
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DMA-END] ln=%d cyc=%d mode=%d addr=%04X\n",
                             scanline,
                             lineCycles,
                             dmaMode,
                             dmaAddress);
            }
        }
    } else if (dmaMode == 3) {
        // Copy: 2 external slots per byte.
        // Uses dmaAddress for the same reason as fill.
        if (!dmaCopySecondSlot) {
            dmaCopySecondSlot = true;
            return;
        }
        dmaCopySecondSlot = false;

        u16 srcAddr = (static_cast<u16>(regs[22]) << 8) | regs[21];
        flushCurrentLineToCurrentCycle();
        // VRAM copy reads from srcAddr, writes to dmaAddress ^ 1
        vram[(dmaAddress ^ 1) & 0xFFFF] = vram[srcAddr & 0xFFFF];
        srcAddr++;
        regs[21] = srcAddr & 0xFF;
        regs[22] = (srcAddr >> 8) & 0xFF;
        dmaAddress = static_cast<u16>(dmaAddress + regs[15]);

        u16 lengthCounter = (static_cast<u16>(regs[20]) << 8) | regs[19];
        lengthCounter = static_cast<u16>(lengthCounter - 1);
        regs[19] = lengthCounter & 0xFF;
        regs[20] = (lengthCounter >> 8) & 0xFF;

        dmaWordsRemaining--;
        if (dmaWordsRemaining == 0) {
            dmaActive = false;
            address = dmaAddress;
            regs[19] = 0;
            regs[20] = 0;
            static const bool logHintTiming = []() {
                const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
                return env && std::atoi(env) != 0;
            }();
            if (logHintTiming) {
                std::fprintf(stderr,
                             "[DMA-END] ln=%d cyc=%d mode=%d addr=%04X\n",
                             scanline,
                             lineCycles,
                             dmaMode,
                             dmaAddress);
            }
        }
    }
}
