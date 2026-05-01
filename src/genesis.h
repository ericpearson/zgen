// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "types.h"
#include "cpu/m68k.h"
#include "cpu/z80.h"
#include "video/vdp.h"
#include "memory/bus.h"
#include "memory/cartridge.h"
#include "audio/ym2612.h"
#include "audio/psg.h"
#include "cheats/cheat_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Cheats {
class CheatEngine;
class CheatMemoryInterface;
}
class GenesisCheatMemoryBridge;

class Genesis {
public:
    using CheatEntry = Cheats::CheatEntry;
    using CheatValueType = Cheats::CheatValueType;
    using SearchCandidate = Cheats::SearchCandidate;
    using SearchCompareMode = Cheats::SearchCompareMode;
    using SearchHeuristic = Cheats::SearchHeuristic;

    struct FrameProfile {
        double cheatsMs = 0.0;
        double m68kMs = 0.0;
        double z80Ms = 0.0;
        double vdpMs = 0.0;
        double ymMs = 0.0;
        double psgMs = 0.0;
        double mixMs = 0.0;
    };

    struct BadSplitTraceEntry {
        int frame = 0;
        int scanline = 0;
        int lineCycle = 0;
        int cycles = 0;
        int pendingInterrupt = -1;
        u32 pc = 0;
        u32 nextPc = 0;
        u16 ir = 0;
        u16 sr = 0;
    };

    Genesis();
    ~Genesis();
    
    bool loadROM(const char* filename);
    void reset();
    void runFrame();
    void step();  // Single CPU instruction
    
    // Component access
    M68K& getCPU() { return m68k; }
    M68K& getM68K() { return m68k; }
    Z80& getZ80() { return z80; }
    VDP& getVDP() { return vdp; }
    Bus& getBus() { return bus; }
    Cartridge& getCartridge() { return cartridge; }
    const Cartridge& getCartridge() const { return cartridge; }
    
    u32* getFramebuffer() { return vdp.getFramebuffer(); }
    int getScreenWidth() const { return 320; }
    int getFramebufferPitch() const { return 320; }
    int getViewportWidth() const { return vdp.activeWidth; }
    int getViewportXOffset() const { return (320 - vdp.activeWidth) / 2; }
    int getScreenHeight() const { return vdp.getOutputHeight(); }
    
    // Audio
    const s16* getAudioBuffer() const { return audioBuffer; }
    int getAudioSamples() const { return audioBufferPos / 2; }  // stereo pairs
    void setAudioSampleRate(int rate);
    void setVideoStandard(VideoStandard standard);
    void syncYMBeforeWrite();  // Advance YM to current Z80 position before register write
    VideoStandard getVideoStandard() const { return vdp.getVideoStandard(); }
    double getFrameRate() const;
    int getScanlinesPerFrame() const { return vdp.getTotalScanlines(); }
    bool hasROMLoaded() const { return cartridge.isLoaded(); }

    // Input
    void setButton(int port, int button, bool pressed);

    // Cheats
    bool addCheat(const std::string& code, const std::string& name, std::string* errorText = nullptr);
    bool toggleCheat(size_t index);
    bool removeCheat(size_t index);
    void clearCheats();
    const std::vector<CheatEntry>& getCheats() const;
    bool loadCheatsFromFile(std::string* errorText = nullptr, size_t* loadedCount = nullptr);
    bool saveCheatsToFile(std::string* errorText = nullptr) const;
    std::string getCheatPath() const;

    // RAM search
    void resetRamSearch();
    bool startRamSearchKnownValue(CheatValueType type, s32 targetValue, SearchHeuristic heuristic);
    bool startRamSearchUnknown(CheatValueType type);
    bool refineRamSearch(SearchCompareMode mode, std::optional<s32> compareValue, SearchHeuristic heuristic);
    size_t getRamSearchResultCount() const;
    void getRamSearchResults(size_t offset, size_t limit, std::vector<SearchCandidate>& out) const;
    bool getRamSearchResult(size_t index, SearchCandidate& out) const;
    bool setRamSearchResultValueOnce(size_t index, u32 value);
    bool freezeRamSearchResult(size_t index, u32 value, const std::string& name, std::string* errorText = nullptr);
    bool refreshRamSearchResultValue(size_t index, u32& outValue) const;
    bool isRamSearchActive() const;
    CheatValueType getRamSearchValueType() const;
    
    // Timing constants
    static constexpr int NTSC_MASTER_CLOCK = 53693175;
    static constexpr int PAL_MASTER_CLOCK = 53203424;
    static constexpr int M68K_DIVIDER = 7;
    static constexpr int MASTER_CYCLES_PER_SCANLINE = 3420;  // Exact NTSC H32/H40
    static constexpr int M68K_CYCLES_PER_SCANLINE = MASTER_CYCLES_PER_SCANLINE / M68K_DIVIDER;  // 488 (remainder handled by accumulator)
    
    // Save states
    bool saveState(int slot, const u32* screenshotBuffer = nullptr);
    bool loadState(int slot);
    bool loadStateFromFile(const std::string& path);
    bool hasState(int slot) const;
    int findLatestStateSlot() const;
    bool loadStateThumbnail(int slot, u32* thumbnailOut) const;
    std::string getStatePath(int slot) const;
    void setRomPath(const std::string& path) { currentRomPath_ = path; }
    bool dumpVdpStateBin(const std::string& path, bool includeM68kState = true) const;
    void scheduleFrameBoundaryVdpDump(const std::string& path, bool includeM68kState = true) {
        pendingFrameBoundaryVdpDumpPath_ = path;
        pendingFrameBoundaryVdpDumpIncludeM68kState_ = includeM68kState;
    }

    // Debug
    bool isPaused() const { return paused; }
    void setPaused(bool p) { paused = p; }
    int getFrameCount() const { return frameCount; }
    const FrameProfile& getFrameProfile() const { return frameProfile_; }
    void setDetailedProfiling(bool enabled);
    
    // Z80 timing
    static constexpr int Z80_DIVIDER = 15;
    
private:
    friend class GenesisCheatMemoryBridge;

    M68K m68k;
    Z80 z80;
    VDP vdp;
    Bus bus;
    Cartridge cartridge;
    YM2612 ym2612;
    PSG psg;
    
    int frameCount;
    bool paused;
    bool detailedProfilingEnabled_;
    FrameProfile frameProfile_;

    // Audio output buffer (stereo interleaved: L, R, L, R, ...)
    static constexpr int DEFAULT_AUDIO_RATE = 48000;
    static constexpr int AUDIO_BUFFER_SIZE = 4096;  // interleaved s16 values
    int audioRate;
    double audioSamplesPerScanline;
    double ymTicksPerOutputSample;
    s16 audioBuffer[AUDIO_BUFFER_SIZE];
    int audioBufferPos;
    double audioSampleCounter;  // fractional accumulator for even sample distribution
    double ymOutputTickCounter; // fractional accumulator for YM2612 native ticks per host output sample
    int z80CycleAccum;          // fractional accumulator for M68K-to-Z80 clock conversion
    int ymNativeTickAccum;      // fractional accumulator for M68K-to-YM2612 native sample conversion
    int ymPendingCycles_;       // M68K cycles of YM output pending in current burst
    int ymBurstTotalCycles_;    // total M68K cycles in current Z80 burst (for clamping)
    int ymBurstDrained_;        // M68K cycles of YM already drained this burst (via syncs)
    int z80BurstInitialDebt_;   // z80CycleDebt at start of current burst (for progress calc)
    int m68kCycleDebt;          // carry-over from scanline overshoot (negative = overshoot)
    int z80CycleDebt;           // persistent converted Z80 cycle budget (negative = overshoot)
    int m68kLineAccum;          // fractional M68K cycles accumulator (3420 master / 7 has remainder)
    // Per-frame stall counters (diagnostic): accumulated 68K cycles where the
    // CPU was stalled for DMA-bus-hold or FIFO-full; reset at frame start and
    // logged via GENESIS_LOG_STALLS=1.
    u64 stallCyclesDMA_ = 0;
    u64 stallCyclesFIFO_ = 0;
    static constexpr int BAD_SPLIT_TRACE_RING_SIZE = 16384;
    BadSplitTraceEntry badSplitTraceRing_[BAD_SPLIT_TRACE_RING_SIZE];
    int badSplitTraceWriteIndex_;
    int badSplitTraceCount_;
    u32 badSplitTraceSeenSerial_;
    bool badSplitTraceDumped_;
    bool badSplitTraceActive_;
    bool badSplitTraceComplete_;
    bool badSplitTraceSawNormalSplit_;

    static constexpr int YM_NATIVE_QUEUE_SIZE = 4096;
    YMSample ymNativeQueue[YM_NATIVE_QUEUE_SIZE];
    int ymNativeQueueHead;
    int ymNativeQueueTail;
    int ymNativeQueueCount;
    YMSample ymLastSample;

    // DC-blocking filter state (emulates hardware AC coupling)
    double dcBlockPrevIn[2];   // Previous input sample (L, R)
    double dcBlockPrevOut[2];  // Previous output sample (L, R)

    // YM2612 low-pass filter state (emulates Model 1 analog RC filter ~3390 Hz)
    double ymLpfPrevIn[2];    // Previous input sample (L, R)
    double ymLpfPrevOut[2];   // Previous output sample (L, R)

    // Controller state
    u8 controllerData[2];
    u8 controllerCtrl[2];

    std::unique_ptr<Cheats::CheatMemoryInterface> cheatMemory_;
    std::unique_ptr<Cheats::CheatEngine> cheatEngine_;

    std::string pendingFrameBoundaryVdpDumpPath_;
    bool pendingFrameBoundaryVdpDumpIncludeM68kState_ = true;
    std::string currentRomPath_;

    void initCheatEngine();
    int masterClockHz() const;
    void clearYMSampleQueue();
    void pushYMSample(const YMSample& sample);
    bool popYMSample(YMSample& outSample);
    void clockYM(int m68kCycles);
    void clockZ80(int m68kCycles);
    bool cheatRead8(u32 address, u8& outValue) const;
    bool cheatRead16(u32 address, u16& outValue) const;
    bool cheatWrite8(u32 address, u8 value);
    bool cheatWrite16(u32 address, u16 value);
    void cheatEnumerateWritableRanges(std::vector<Cheats::MemoryRange>& out) const;
    void resetBadSplitTraceState();
    void appendBadSplitTraceEntry(int scanline, int lineCycle, int cycles);
    void checkBadSplitTraceEvents();
    void dumpBadSplitTraceHistory(int disableLine) const;
    bool canonicalizeLoadedBoundaryState();
    void runScanline();
};
