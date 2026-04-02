// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include "video/vdp_slot_table.h"

class Bus;

enum VDPCode {
    CODE_VRAM_READ = 0,
    CODE_VRAM_WRITE = 1,
    CODE_CRAM_WRITE = 3,
    CODE_VSRAM_READ = 4,
    CODE_VSRAM_WRITE = 5,
    CODE_CRAM_READ = 8
};

struct SpriteEntry {
    s16 ypos;
    u8 width;
    u8 height;
    u8 link;
    bool priority;
    u8 palette;
    bool hflip;
    bool vflip;
    u16 pattern;
    s16 xpos;
};

struct VDPFifoEntry {
    u16 value = 0;
    u16 address = 0;
    u8 code = 0;
    bool byteWrite = false;
    bool lowByte = false;
};

struct CRAMDotEvent {
    int pixel = 0;    // Screen X position
    u32 color = 0;    // RGB color to overlay
};

struct VDPDisplayEnableEvent {
    bool value = false;
    int scanline = 0;
    int pixel = 0;
};

class VDP {
public:
    VDP();
    void connectBus(Bus* b) { bus = b; }
    void reset();

    // Port access
    u16 readData();
    u16 readControl();
    u16 readControl(int partialCycles);  // Cycle-offset-aware status read
    u16 readHVCounter();
    void writeData(u16 value);
    void writeDataByte(u8 value, bool lowByte);
    void writeControl(u16 value);
    void setVideoStandard(VideoStandard standard);
    VideoStandard getVideoStandard() const { return videoStandard_; }
    int getTotalScanlines() const;

    // Rendering
    void beginScanline();
    void endScanline();
    void clockM68K(int cycles);
    void setCpuTimingBaseLineCycle(int cycle) { cpuTimingBaseLineCycle_ = cycle; }
    void syncToCpuTimingOffset(int partialCycles);
    void syncToLineCycle(int targetCycle);
    int nextCpuEventLineCycle() const;
    u32* getFramebuffer() { return framebuffer; }

    // Interrupts
    bool vblankPending() const { return vblankIRQ; }
    bool hblankPending() const { return hblankIRQ; }
    bool hblankIRQAsserted() const { return hblankIRQ && hblankEnabled; }
    void clearVBlank() { vblankIRQ = false; }
    void clearHBlank() { hblankIRQ = false; }
    void acknowledgeInterrupt(int level);
    void relatchVsramSnapshot() { vsramSnapshot[0] = vsram[0]; vsramSnapshot[1] = vsram[1]; }
    void drainPendingFIFOForSnapshot();

    // Status
    int getScanline() const { return scanline; }
    bool inVBlank() const { return scanline >= activeHeight; }
    bool is68kDMABusy() const { return dmaActive && dmaMode <= 1 && dmaWordsRemaining > 0; }
    bool isVDPFIFOFull() const { return fifoCount >= FIFO_SIZE; }
    int fifoWaitCycles() const;
    int dmaWaitCycles() const;
    int fifoCountAtCycleOffset(int offset) const;

    // Memory access for DMA
    u8 readVRAM(u16 addr) const { return vram[addr & 0xFFFF]; }
    void writeVRAM(u16 addr, u8 val) { vram[addr & 0xFFFF] = val; }

    // Debug access
    u8 getReg(int i) const { return regs[i]; }
    const u8* getVRAM() const { return vram; }
    const u16* getCRAM() const { return cram; }
    const u16* getVSRAM() const { return vsram; }
    const u8* getRegs() const { return regs; }
    u32 getDisplayEnableApplySerial() const { return displayEnableApplySerial_; }
    int getLastDisplayEnableApplyScanline() const { return lastDisplayEnableApplyScanline_; }
    int getLastDisplayEnableApplyPixel() const { return lastDisplayEnableApplyPixel_; }
    bool getLastDisplayEnableApplyValue() const { return lastDisplayEnableApplyValue_; }

    friend class Genesis;

private:
    Bus* bus;

    // VRAM, CRAM, VSRAM
    u8 vram[0x10000];     // 64KB
    u16 cram[64];         // 64 colors (9-bit stored in 16-bit)
    u32 cramColors[64];
    u32 cramShadowColors[64];
    u32 cramHighlightColors[64];
    u16 vsram[40];        // 40 words
    u16 vsramSnapshot[2];  // Line-start VSRAM latch for full-screen vscroll [0]=A, [1]=B
    u16 hscrollSnapshot[2]; // Line-start HSCROLL latch [0]=Plane A, [1]=Plane B

    // Registers
    u8 regs[24];

    // Control port state
    bool controlPending;
    u16 controlLatch;
    u8 code;
    u16 address;
    bool cachedControlWritePending;
    u16 cachedControlWriteValue;

    // DMA state
    bool dmaActive;
    u8 dmaMode;
    u32 dmaWordsRemaining;  // Remaining words for 68K->VDP DMA
    bool dmaFillPending;    // Waiting for data port write to start fill
    u16 dmaFillValue;       // Fill DMA value (full word from data port write)
    u8 dmaFillCode;         // Latched code at fill start (target won't change mid-DMA)
    bool dmaVramSecondSlot;  // 68K→VRAM DMA: second-slot tracking
    bool dmaCopySecondSlot;  // Copy DMA: second-slot tracking
    u16 dmaAddress;          // Separate DMA destination counter (fill/copy)

    // FIFO state
    static constexpr int FIFO_SIZE = 4;
    VDPFifoEntry fifo[FIFO_SIZE];
    int fifoCount;
    int fifoReadIndex;
    int fifoWriteIndex;
    bool vramWriteSecondSlot; // FIFO VRAM writes take 2 external slots

    // Mid-line display-enable transitions. Under the current segmented renderer,
    // reg #1 bit 6 changes are queued to the next 2-pixel VDP slot rather than
    // being forced to the next scanline.
    static constexpr int DISPLAY_ENABLE_EVENT_QUEUE_SIZE = 8;
    VDPDisplayEnableEvent displayEnableEvents[DISPLAY_ENABLE_EVENT_QUEUE_SIZE];
    int displayEnableEventCount;
    int displayEnableEventReadIndex;
    int displayEnableEventWriteIndex;

    // CRAM dot artifacts (env-gated behind GENESIS_CRAM_DOTS=1).
    // When a CRAM write occurs during active display, the VDP briefly outputs
    // the written color data instead of the normal pixel.
    static constexpr int MAX_CRAM_DOTS = 16;
    CRAMDotEvent cramDots[MAX_CRAM_DOTS];
    int cramDotCount;

    // Rendering state
    int scanline;
    int hcounter;
    int lineCycles;
    int lineCycleBudget;   // M68K cycles available this scanline (488/489 phase-adjusted)
    int cpuTimingBaseLineCycle_;
    int currentSlotIndex;   // Position within the scanline's slot table
    int hblankStartCycle_;          // M68K cycle at which HBlank starts (for H-int timing)
    bool hblankCounterDecremented_; // Already processed H-counter this line
    bool lineIsBlank;       // Current line renders blank: VBlank or display disabled before visible output
    bool oddFrame;          // Alternates each frame for interlace
    int debugFrameNumber_;  // Debug-only frame tag for trace logging
    int renderedPixels;     // Current line rendered up to this pixel frontier
    u32 framebuffer[320 * 480]; // Expanded for interlace modes (320x448/480)
    u16 scanlineColorBuffer[320];
    u8 scanlineRankBuffer[320];
    bool spritePixelBuffer[320];
    static constexpr int MAX_SPRITES_PER_LINE = 20;
    SpriteEntry spriteLineCache[2][MAX_SPRITES_PER_LINE];
    int spriteLineCount[2];
    bool spriteLineValid[2];
    int spriteLineNumber[2];

    // Interrupt state
    bool vblankIRQ;       // V-int pending — drives /VINT to 68K, cleared by INT ACK only
    bool vblankFlag;      // Status register F flag (bit 7) — cleared by status read
    bool vblankIRQArmed;  // Waiting to assert /VINT later within the first VBlank line
    int vblankIRQAssertCycle;
    bool hblankIRQ;      // Raw H-int pending latch, cleared by INT ACK only
    int hblankCounter;

    // Status register flags
    bool spriteCollision;   // Two non-transparent sprite pixels overlap
    bool spriteOverflow;    // Max sprites per line exceeded
    bool sprMaskSeen_ = false; // Sprite X=0 masking: non-zero X sprite seen on current line

    // HV counter latching
    u16 hvLatch;
    bool hvLatched;

    // Decoded register values
    int activeWidth;      // 256 or 320
    int activeHeight;     // 224 or 240
    bool displayEnabled;
    bool displayEnabledLatch;  // Last written value from reg #1 bit 6
    u32 displayEnableApplySerial_;
    int lastDisplayEnableApplyScanline_;
    int lastDisplayEnableApplyPixel_;
    bool lastDisplayEnableApplyValue_;
    bool vblankEnabled;
    bool hblankEnabled;
    bool dmaEnabled;
    bool shadowHighlightEnabled;
    u8 interlaceMode;     // 0=normal, 1=interlace, 2=unused, 3=double resolution
    u8 interlaceModeLatch; // Pending value; applied at scanline boundary
    VideoStandard videoStandard_;
    u16 scrollABase;
    u16 scrollBBase;
    u16 windowBase;
    u16 spriteBase;
    u16 hscrollBase;
    int hscrollMode;      // 0=full, 2=cell, 3=line
    int vscrollMode;      // 0=full, 1=2cell
    u8 bgColorIndex;
    int scrollWidth;      // 32, 64, or 128
    int scrollHeight;     // 32, 64, or 128

    // Internal methods
    void updateRegisters();
    void decodeRegister(int reg);
    void drawCurrentLine();
    bool inHBlankPeriod() const;
    bool inHBlankPeriod(int cycleOffset) const;  // With partial instruction cycles
    void beginScanlineCommon();
    void beginScanlineTimed();
    void endScanlineTimed();
    int currentVisiblePixel() const;
    int visiblePixelAtLineCycle(int cycle) const;
    int m68kCycleForVisiblePixel(int pixel) const;
    int nextVisibleSlotBoundaryPixel() const;
    void flushCurrentLineToCurrentCycle();
    void flushCurrentLineToPixel(int pixelEnd);
    bool effectiveDisplayEnabled() const;
    void recordDisplayEnableApply(int scanline, int pixel, bool value);
    void scheduleDisplayEnableChange(bool value);
    void updateCachedColor(int index);
    void refreshCachedColors();
    void refreshVBlankIRQState();
    bool enqueueFIFOEntry(u16 value, u16 writeAddress, u8 writeCode, bool byteWrite, bool lowByte);
    void applyFIFOEntry(const VDPFifoEntry& entry);
    const VDPSlotTable& currentSlotTable() const;
    int slotIndexForM68kCycle(int cycle) const;
    void processExternalSlot();
    void processDMASlot();
    void megaTimingLog(const char* tag) const;
    void renderScanline(int line);
    void renderScanlineSegment(int line, int startX, int endX);
    void renderBackground(int line, int layer, int startX, int endX, u16* lineBuffer, u8* prioBuffer);
    void renderSprites(int line, int startX, int endX, u16* lineBuffer, u8* prioBuffer);
    void renderWindow(int line, int startX, int endX, u16* lineBuffer, u8* prioBuffer);
    u32 cramToRGB(u16 cramValue);
    u32 cramToRGBShadow(u16 cramValue);
    u32 cramToRGBHighlight(u16 cramValue);

    // DMA operations
    void executeDMA();
    void dma68kToVRAM();

    // Sprite helpers
    void parseSprites(int line, SpriteEntry* sprites, int& count);
};
