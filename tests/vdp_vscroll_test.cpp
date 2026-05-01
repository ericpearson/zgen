// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only
//
// Per-column vscroll renderer and capture tests.
//
// The per-column vscroll path has two halves:
//   1. Capture: SLOT_NT_A fires per column at each column's NT fetch slot
//      and stores vsram[0/1] into columnVscroll[col][layer].
//   2. Consume: renderBackground() and fetchColumnTiles() read per-column
//      from columnVscroll[col][layer] for H40 full-screen vscroll mode.
//
// Mid-line capture is driven by the slow-path slot loop which only runs
// when the FIFO is active (CPU writes to VDP). That path cannot be
// exercised in a unit test without a CPU. What we CAN test directly:
//   - The line-start pre-fill of columnVscroll[] from vsram[] in
//     beginScanlineCommon(). Runs unconditionally.
//   - The renderer's consumption of columnVscroll[] — poke per-column
//     values, render, confirm per-column values are reflected in output.
// These together prove the per-column pipeline is wired end to end for
// the single-frame case. Scene-level integration (mid-line capture during
// Cotton's per-line H-int stream) is exercised by the existing Cotton
// regression tests in tests/test_regressions.sh (boot pink-line, intro
// stripe, HUD/play-field flicker) — those pass only when per-column
// dispatch is correct.

#include "video/vdp.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

static int totalTests = 0;
static int passedTests = 0;
static int failedTests = 0;
static const char* currentGroup = nullptr;

static void beginGroup(const char* name) {
    currentGroup = name;
    std::printf("  %s\n", name);
}

static void endGroup() {}

static bool check(bool condition, const char* desc) {
    totalTests++;
    if (condition) {
        passedTests++;
        return true;
    }
    failedTests++;
    std::printf("    FAIL [%s]: %s\n", currentGroup ? currentGroup : "", desc);
    return false;
}

class VdpVscrollTest {
public:
    // Configure H40 full-screen vscroll mode. Bypasses bus setup by
    // writing registers directly and calling decodeRegister for each.
    static void configureH40FullScreen(VDP& vdp) {
        vdp.regs[0x00] = 0x04;   // R0: H-int disabled (no CPU in test)
        vdp.regs[0x01] = 0x64;   // R1: display on, M5, V28
        vdp.regs[0x02] = 0x08;   // R2: plane A base 0x2000
        vdp.regs[0x04] = 0x02;   // R4: plane B base 0x4000
        vdp.regs[0x05] = 0x7C;   // R5: SAT base (not used here)
        vdp.regs[0x07] = 0x00;   // R7: backdrop = CRAM[0]
        vdp.regs[0x0B] = 0x03;   // R11: full-line HSCROLL, full-screen VSCROLL
        vdp.regs[0x0C] = 0x81;   // R12: H40, no shadow/highlight
        vdp.regs[0x10] = 0x01;   // R16: plane 64x32
        for (int i = 0; i < 24; i++) vdp.decodeRegister(i);
        vdp.displayEnabled = true;
        vdp.displayEnabledLatch = true;
    }

    static void setVsram(VDP& vdp, int index, uint16_t value) {
        vdp.vsram[index & 0x3F] = value;
    }

    static void setColumnVscroll(VDP& vdp, int col, int layer, uint16_t value) {
        vdp.columnVscroll[col][layer] = value;
    }

    static uint16_t getColumnVscroll(const VDP& vdp, int col, int layer) {
        return vdp.columnVscroll[col][layer];
    }

    static void setVscrollLatch(VDP& vdp, int layer, uint16_t value) {
        vdp.vscrollLatch[layer] = value;
        vdp.visibleLineVscroll[layer] = value;
    }

    static uint16_t getVscrollLatch(const VDP& vdp, int layer) {
        return vdp.vscrollLatch[layer];
    }

    static void setScanline(VDP& vdp, int line) {
        vdp.scanline = line;
    }

    static int getRenderedPixels(const VDP& vdp) {
        return vdp.renderedPixels;
    }

    static void moveToH40Slot(VDP& vdp, int slot) {
        vdp.currentSlotIndex = slot;
        vdp.lineCycles = (kH40Active.mclkOffset[slot] + 6) / 7;
        vdp.hcounter = kH40Active.hcounterAtSlot[slot];
    }

    static void applyVsramWord(VDP& vdp, int index, uint16_t value) {
        VDPFifoEntry entry{};
        entry.value = value;
        entry.address = static_cast<uint16_t>(index * 2);
        entry.code = CODE_VSRAM_WRITE;
        entry.enqueueScanline = vdp.scanline;
        entry.enqueueLineCycle = 0;
        vdp.applyFIFOEntry(entry);
    }

    static void applyCarryoverVsramWord(VDP& vdp, int index, uint16_t value) {
        VDPFifoEntry entry{};
        entry.value = value;
        entry.address = static_cast<uint16_t>(index * 2);
        entry.code = CODE_VSRAM_WRITE;
        entry.enqueueScanline = vdp.scanline - 1;
        entry.enqueueLineCycle = VDP_MAX_M68K_CYCLES - 1;
        vdp.applyFIFOEntry(entry);
    }

    // Write a word directly to VRAM, byte-swapped (VDP stores BE).
    static void writeVramWord(VDP& vdp, uint16_t addr, uint16_t value) {
        vdp.vram[addr & 0xFFFF]       = static_cast<uint8_t>(value >> 8);
        vdp.vram[(addr + 1) & 0xFFFF] = static_cast<uint8_t>(value & 0xFF);
    }

    // Stamp a specific color index into a tile pattern row (8 pixels of
    // identical color). py is 0..7. Writes at vram[tile*32 + py*4].
    static void setTilePatternSolidColor(VDP& vdp, int tile, int py, uint8_t colorIndex) {
        uint8_t packed = static_cast<uint8_t>((colorIndex << 4) | colorIndex);
        uint16_t addr = static_cast<uint16_t>(tile * 32 + py * 4);
        for (int b = 0; b < 4; b++) {
            vdp.vram[(addr + b) & 0xFFFF] = packed;
        }
    }

    static void setCram(VDP& vdp, int index, uint16_t cramValue) {
        vdp.cram[index] = cramValue;
        vdp.updateCachedColor(index);
    }

    static uint32_t getFramebufferPixel(const VDP& vdp, int x, int y) {
        return vdp.framebuffer[y * 320 + x];
    }

    // Render a single scanline to the framebuffer using the internal
    // slot-independent path. No bus, no FIFO, no CPU — just the render
    // pipeline on current VDP state.
    static void renderLine(VDP& vdp, int line) {
        vdp.renderScanlineSegment(line, 0, vdp.activeWidth);
    }

    static void configureVIntLine(VDP& vdp, bool enabled) {
        vdp.regs[0x01] = static_cast<uint8_t>((vdp.regs[0x01] & ~0x20) | (enabled ? 0x20 : 0x00));
        vdp.decodeRegister(0x01);
        vdp.activeWidth = 320;
        vdp.activeHeight = 224;
        vdp.scanline = 224;
        vdp.lineCycles = 0;
        vdp.vblankIRQ = false;
        vdp.vblankIRQArmed = false;
        vdp.vblankIRQAssertCycle = 0;
        vdp.refreshVBlankIRQState();
    }

    static void setVIntEnabled(VDP& vdp, bool enabled) {
        vdp.regs[0x01] = static_cast<uint8_t>((vdp.regs[0x01] & ~0x20) | (enabled ? 0x20 : 0x00));
        vdp.decodeRegister(0x01);
    }

    static bool rawVBlankIRQ(const VDP& vdp) {
        return vdp.vblankIRQ;
    }

    static bool rawHBlankIRQ(const VDP& vdp) {
        return vdp.hblankIRQ;
    }

    static void configureHIntLine(VDP& vdp) {
        configureH40FullScreen(vdp);
        vdp.regs[0x00] = 0x14;   // H-int enabled
        vdp.regs[0x0A] = 0x00;   // Reload to zero: every active line asserts
        vdp.decodeRegister(0x00);
        vdp.decodeRegister(0x0A);
        vdp.scanline = 23;
        vdp.activeHeight = 224;
        vdp.hblankCounter = 0;
        vdp.hblankIRQ = false;
    }

    static bool vblankIRQArmed(const VDP& vdp) {
        return vdp.vblankIRQArmed;
    }

    static int vblankIRQAssertCycle(const VDP& vdp) {
        return vdp.vblankIRQAssertCycle;
    }
};

// Line-start pre-fill: every entry of columnVscroll[] is set to current
// vsram[0/1] when beginScanlineCommon runs. Any mid-line capture must
// overwrite onto this pre-filled baseline. Clobber the array before the
// call to prove the pre-fill is doing the work.
static void testBeginScanlinePrefillFromVsram() {
    beginGroup("beginScanline pre-fills columnVscroll[] from vsram[0/1]");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureH40FullScreen(vdp);
    VdpVscrollTest::setVsram(vdp, 0, 0x123);
    VdpVscrollTest::setVsram(vdp, 1, 0x456);
    // Clobber all columns with a sentinel so we can see the pre-fill work.
    for (int c = 0; c < 42; c++) {
        VdpVscrollTest::setColumnVscroll(vdp, c, 0, 0xFFFF);
        VdpVscrollTest::setColumnVscroll(vdp, c, 1, 0xFFFF);
    }
    vdp.beginScanline();
    for (int c = 0; c < 42; c++) {
        char desc[96];
        std::snprintf(desc, sizeof(desc),
                      "col %d plane A pre-filled to vsram[0]=0x123", c);
        check(VdpVscrollTest::getColumnVscroll(vdp, c, 0) == 0x123, desc);
        std::snprintf(desc, sizeof(desc),
                      "col %d plane B pre-filled to vsram[1]=0x456", c);
        check(VdpVscrollTest::getColumnVscroll(vdp, c, 1) == 0x456, desc);
    }
    endGroup();
}

// H40 samples VSRAM[0]/[1] once per line into vscroll_latch when full-screen
// vscroll is active. Our renderer reads `vscrollLatch[layer]`, not a
// per-column buffer. Prove the latch path is active: line-wide uniform
// vscroll must produce uniform output even if columnVscroll[] is poked to
// divergent per-column values.
static void testRendererUsesLineWideVscrollLatchForH40FullScreen() {
    beginGroup("renderBackground uses line-wide vscrollLatch for H40 full-screen");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureH40FullScreen(vdp);

    VdpVscrollTest::setTilePatternSolidColor(vdp, 1, 0, 5);  // tile 1 row 0 = color 5 (red)
    VdpVscrollTest::setTilePatternSolidColor(vdp, 2, 0, 9);  // tile 2 row 0 = color 9 (blue)
    const uint16_t NT_B = 0x4000;
    for (int c = 0; c < 64; c++) {
        VdpVscrollTest::writeVramWord(vdp, NT_B + (0 * 64 + c) * 2, 0x0001);
        VdpVscrollTest::writeVramWord(vdp, NT_B + (1 * 64 + c) * 2, 0x0002);
    }
    VdpVscrollTest::setCram(vdp, 0, 0x0000);
    VdpVscrollTest::setCram(vdp, 5, 0x000E);  // red
    VdpVscrollTest::setCram(vdp, 9, 0x0E00);  // blue

    // Set up divergent columnVscroll[] values. If the renderer reads per-
    // column (old Cotton-design path), output would mix red + blue across
    // the line. With the line-wide latch path, columnVscroll[] is ignored.
    for (int c = 0; c < 20; c++) {
        uint16_t v = (c <= 9) ? 0 : 8;
        VdpVscrollTest::setColumnVscroll(vdp, c, 0, v);
        VdpVscrollTest::setColumnVscroll(vdp, c, 1, v);
    }

    vdp.beginScanline();
    // Line-wide latch controls rendering — set it to vscroll=0 so plane B
    // row 0 (tile 1 = color 5 = red) is used for the whole line.
    VdpVscrollTest::setVscrollLatch(vdp, 0, 0);
    VdpVscrollTest::setVscrollLatch(vdp, 1, 0);
    VdpVscrollTest::renderLine(vdp, 0);

    int redCols = 0;
    for (int colIdx = 0; colIdx < 20; colIdx++) {
        int x = colIdx * 16 + 8;
        uint32_t px = VdpVscrollTest::getFramebufferPixel(vdp, x, 0);
        uint8_t r = (px >> 16) & 0xFF;
        uint8_t b = px & 0xFF;
        if (r > b && r > 64) redCols++;
    }
    {
        char desc[160];
        std::snprintf(desc, sizeof(desc),
                      "all 20 column pairs render red (latch=0 -> tile 1): got %d/20",
                      redCols);
        check(redCols >= 19, desc);
    }

    // Flip latch to vscroll=8 (plane B row 1 -> tile 2 -> blue). Per-column
    // buffer is still divergent; it must still be ignored.
    vdp.beginScanline();
    VdpVscrollTest::setVscrollLatch(vdp, 0, 8);
    VdpVscrollTest::setVscrollLatch(vdp, 1, 8);
    VdpVscrollTest::renderLine(vdp, 0);

    int blueCols = 0;
    for (int colIdx = 0; colIdx < 20; colIdx++) {
        int x = colIdx * 16 + 8;
        uint32_t px = VdpVscrollTest::getFramebufferPixel(vdp, x, 0);
        uint8_t r = (px >> 16) & 0xFF;
        uint8_t b = px & 0xFF;
        if (b > r && b > 64) blueCols++;
    }
    {
        char desc[160];
        std::snprintf(desc, sizeof(desc),
                      "all 20 column pairs render blue (latch=8 -> tile 2): got %d/20",
                      blueCols);
        check(blueCols >= 19, desc);
    }
    endGroup();
}

// H40 samples full-screen vscroll at line change. In our line coordinate,
// HINT-driven VSRAM writes can drain from the FIFO early in the following
// line before the hardware-equivalent visible sample. Those writes must
// update the line-wide full-screen sample and invalidate any premature
// segmented output; writes
// after the sample window must not retroactively change the line sample.
static void testH40EarlyVsramDrainUpdatesFullScreenLineSample() {
    beginGroup("H40 full-screen vscroll sample follows early FIFO VSRAM drains");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureH40FullScreen(vdp);
    VdpVscrollTest::setVsram(vdp, 0, 0x370);
    VdpVscrollTest::setVsram(vdp, 1, 0x370);
    vdp.beginScanline();

    // Simulate an H-int-driven VSRAM pair draining before the H40 line-change
    // sample. OutRun's frame-400 row-24 pair drains in this early H40 window
    // as carry-over FIFO work from the previous scanline.
    VdpVscrollTest::moveToH40Slot(vdp, 45);
    VdpVscrollTest::applyCarryoverVsramWord(vdp, 0, 0x368);
    VdpVscrollTest::moveToH40Slot(vdp, 55);
    VdpVscrollTest::applyCarryoverVsramWord(vdp, 1, 0x368);

    check(VdpVscrollTest::getVscrollLatch(vdp, 0) == 0x368,
          "pre-sample VSRAM[0] drain updates H40 line-wide vscroll latch");
    check(VdpVscrollTest::getVscrollLatch(vdp, 1) == 0x368,
          "pre-sample VSRAM[1] drain updates H40 line-wide vscroll latch");
    check(VdpVscrollTest::getRenderedPixels(vdp) == 0,
          "pre-sample H40 latch change invalidates premature line rendering");

    // After the visible sample window, VSRAM can change for future lines but
    // must not mutate the already-sampled full-screen vscroll for this line.
    VdpVscrollTest::moveToH40Slot(vdp, 75);
    VdpVscrollTest::applyVsramWord(vdp, 0, 0x360);
    VdpVscrollTest::applyVsramWord(vdp, 1, 0x360);
    check(VdpVscrollTest::getVscrollLatch(vdp, 0) == 0x368,
          "post-sample VSRAM[0] drain does not change current H40 line latch");
    check(VdpVscrollTest::getVscrollLatch(vdp, 1) == 0x368,
          "post-sample VSRAM[1] drain does not change current H40 line latch");
    endGroup();
}

static void testH40CurrentLineVsramWriteDoesNotChangeFullScreenSample() {
    beginGroup("H40 current-line VSRAM writes do not change pre-boundary full-screen sample");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureH40FullScreen(vdp);
    VdpVscrollTest::setVsram(vdp, 0, 0x120);
    VdpVscrollTest::setVsram(vdp, 1, 0x121);
    vdp.beginScanline();

    VdpVscrollTest::moveToH40Slot(vdp, 45);
    VdpVscrollTest::applyVsramWord(vdp, 0, 0x220);
    VdpVscrollTest::moveToH40Slot(vdp, 55);
    VdpVscrollTest::applyVsramWord(vdp, 1, 0x221);

    check(VdpVscrollTest::getVscrollLatch(vdp, 0) == 0x120,
          "current-line VSRAM[0] write does not mutate current H40 line latch");
    check(VdpVscrollTest::getVscrollLatch(vdp, 1) == 0x121,
          "current-line VSRAM[1] write does not mutate current H40 line latch");
    endGroup();
}

static void testH40NextLineUsesBoundaryLatchedVscroll() {
    beginGroup("H40 next line uses VSRAM captured before boundary H-int work");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureH40FullScreen(vdp);
    VdpVscrollTest::setScanline(vdp, 22);
    VdpVscrollTest::setVsram(vdp, 0, 0x040);
    VdpVscrollTest::setVsram(vdp, 1, 0x041);

    vdp.beginScanline();
    vdp.endScanline();
    VdpVscrollTest::setVsram(vdp, 0, 0x140);
    VdpVscrollTest::setVsram(vdp, 1, 0x141);
    vdp.beginScanline();

    check(VdpVscrollTest::getVscrollLatch(vdp, 0) == 0x040,
          "line-wide VSRAM[0] sample was captured before post-boundary writes");
    check(VdpVscrollTest::getVscrollLatch(vdp, 1) == 0x041,
          "line-wide VSRAM[1] sample was captured before post-boundary writes");
    endGroup();
}

static void testVIntSourceLatchesWhileDisabled() {
    beginGroup("V-int source latches independently of enable");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);

    VdpVscrollTest::configureVIntLine(vdp, false);
    check(VdpVscrollTest::vblankIRQArmed(vdp),
          "disabled V-int still arms raw VBlank interrupt source");
    check(!vdp.vblankPending(),
          "disabled V-int source is not visible to the CPU before assertion");

    vdp.clockM68K(VdpVscrollTest::vblankIRQAssertCycle(vdp));
    check(VdpVscrollTest::rawVBlankIRQ(vdp),
          "raw V-int source asserts at the VBlank slot while disabled");
    check(!vdp.vblankPending(),
          "disabled raw V-int source remains masked from CPU delivery");

    VdpVscrollTest::setVIntEnabled(vdp, true);
    check(vdp.vblankPending(),
          "enabling V-int exposes the already-latched source in the same VBlank period");
    endGroup();
}

static void testH40HIntSourceAssertsAtLineBoundary() {
    beginGroup("H40 H-int source asserts at the V-counter line boundary");
    VDP vdp;
    vdp.reset();
    vdp.setVideoStandard(VideoStandard::NTSC);
    VdpVscrollTest::configureHIntLine(vdp);

    vdp.beginScanline();
    check(!VdpVscrollTest::rawHBlankIRQ(vdp),
          "H-int is not pending before the line boundary is clocked");

    vdp.clockM68K(1);
    check(VdpVscrollTest::rawHBlankIRQ(vdp),
          "H-int source asserts immediately after crossing the line boundary");
    check(vdp.hblankIRQAsserted(),
          "enabled H-int source is visible to the 68K after the boundary");
    endGroup();
}


int main() {
    std::printf("VDP vscroll rendering tests\n");
    testBeginScanlinePrefillFromVsram();
    testRendererUsesLineWideVscrollLatchForH40FullScreen();
    testH40EarlyVsramDrainUpdatesFullScreenLineSample();
    testH40CurrentLineVsramWriteDoesNotChangeFullScreenSample();
    testH40NextLineUsesBoundaryLatchedVscroll();
    testVIntSourceLatchesWhileDisabled();
    testH40HIntSourceAssertsAtLineBoundary();

    if (failedTests == 0) {
        std::printf("\nAll %d tests passed\n", totalTests);
        return 0;
    }
    std::printf("\n%d/%d tests failed\n", failedTests, totalTests);
    return 1;
}
