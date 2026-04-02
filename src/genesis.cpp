// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
#include "config_dir.h"
#include "debug_flags.h"
#include "cheats/cheat_engine.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using ProfileClock = std::chrono::steady_clock;

inline double elapsedMs(ProfileClock::time_point start) {
    return std::chrono::duration<double, std::milli>(ProfileClock::now() - start).count();
}

inline bool envEnabled(const char* name) {
    if (!g_debugMode) return false;
    const char* env = std::getenv(name);
    return env && std::atoi(env) != 0;
}

inline int envIntOrDefault(const char* name, int fallback) {
    const char* env = std::getenv(name);
    return env ? std::atoi(env) : fallback;
}

inline bool megaTimingEnabled() {
    return envEnabled("GENESIS_LOG_MEGA_TIMING");
}

inline bool megaTimingShouldLog(int frame, int scanline, int activeHeight) {
    if (!megaTimingEnabled()) {
        return false;
    }
    const int first = envIntOrDefault("GENESIS_LOG_MEGA_FRAME_FIRST", 996);
    const int last = envIntOrDefault("GENESIS_LOG_MEGA_FRAME_LAST", 1002);
    if (frame < first || frame > last) {
        return false;
    }
    return scanline == activeHeight || (scanline >= 246 && scanline <= 255);
}

inline u16 megaTimingRamWord(const Bus& bus, u32 addr) {
    const u8* ram = bus.getRam();
    const u32 offset = addr & 0xFFFF;
    return static_cast<u16>((ram[offset] << 8) | ram[(offset + 1) & 0xFFFF]);
}

inline void logMegaTimingState(const char* tag,
                               int frame,
                               int scanline,
                               int activeHeight,
                               int lineCycles,
                               const VDP& vdp,
                               const M68K& m68k,
                               const Bus& bus,
                               int level = -1) {
    if (!megaTimingShouldLog(frame, scanline, activeHeight)) {
        return;
    }
    std::fprintf(stderr,
                 "[MT-CPU] frame=%d tag=%s ln=%d cyc=%d level=%d pc=%06X sr=%04X pend=%d hpend=%d vpend=%d FF0004=%04X FF04B4=%04X FF04E4=%04X FF07BE=%04X FF116C=%04X\n",
                 frame,
                 tag,
                 scanline,
                 lineCycles,
                 level,
                 m68k.getPC() & 0xFFFFFF,
                 m68k.getSR(),
                 m68k.state.pendingInterrupt,
                 vdp.hblankPending() ? 1 : 0,
                 vdp.vblankPending() ? 1 : 0,
                 megaTimingRamWord(bus, 0xFF0004),
                 megaTimingRamWord(bus, 0xFF04B4),
                 megaTimingRamWord(bus, 0xFF04E4),
                 megaTimingRamWord(bus, 0xFF07BE),
                 megaTimingRamWord(bus, 0xFF116C));
}

inline bool shouldTrackBadSplitTraceLine(int scanline) {
    return scanline >= 210 || scanline <= 125;
}

const char* badSplitTraceLabel(u32 pc) {
    if (pc >= 0x00045E && pc <= 0x000466) return "vint-rte-tail";
    if (pc >= 0x000F54 && pc <= 0x00100A) return "hint-handler";
    if (pc >= 0x00E32C && pc <= 0x00E35C) return "split-gate";
    if (pc >= 0x00DF8A && pc <= 0x00E144) return "split-work";
    return "";
}

} // namespace

Genesis::Genesis() : frameCount(0), paused(false),
                     detailedProfilingEnabled_(false),
                     audioRate(DEFAULT_AUDIO_RATE),
                     audioSamplesPerScanline(0.0), ymTicksPerOutputSample(0.0),
                     audioBufferPos(0), audioSampleCounter(0.0), ymOutputTickCounter(0.0),
                     z80CycleAccum(0), ymNativeTickAccum(0),
                     m68kCycleDebt(0), z80CycleDebt(0), m68kLineAccum(0),
                     badSplitTraceWriteIndex_(0), badSplitTraceCount_(0),
                     badSplitTraceSeenSerial_(0), badSplitTraceDumped_(false),
                     badSplitTraceActive_(false), badSplitTraceComplete_(false),
                     badSplitTraceSawNormalSplit_(false),
                     ymNativeQueueHead(0), ymNativeQueueTail(0), ymNativeQueueCount(0),
                     ymLastSample{},
                     dcBlockPrevIn{}, dcBlockPrevOut{},
                     ymLpfPrevIn{}, ymLpfPrevOut{} {
    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    std::memset(ymNativeQueue, 0, sizeof(ymNativeQueue));
    std::memset(controllerData, 0x7F, sizeof(controllerData));
    std::memset(controllerCtrl, 0, sizeof(controllerCtrl));
    
    // Connect components
    m68k.connectBus(&bus);
    m68k.refreshEnabled_ = true;
    z80.setBus(&bus);
    vdp.connectBus(&bus);
    bus.connectCPU(&m68k);
    bus.connectVDP(&vdp);
    bus.connectZ80(&z80);
    bus.connectYM2612(&ym2612);
    bus.connectPSG(&psg);
    bus.connectCartridge(&cartridge);
    m68k.setInterruptAckCallback([this](int level) {
        logMegaTimingState("IACK", frameCount + 1, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus, level);
        vdp.acknowledgeInterrupt(level);
    });
    initCheatEngine();
    setAudioSampleRate(audioRate);
}

int Genesis::masterClockHz() const {
    return vdp.getVideoStandard() == VideoStandard::PAL ? PAL_MASTER_CLOCK : NTSC_MASTER_CLOCK;
}

double Genesis::getFrameRate() const {
    return static_cast<double>(masterClockHz()) /
           static_cast<double>(MASTER_CYCLES_PER_SCANLINE * getScanlinesPerFrame());
}

void Genesis::setDetailedProfiling(bool enabled) {
    detailedProfilingEnabled_ = enabled;
    if (!enabled) {
        frameProfile_ = {};
    }
}

void Genesis::setAudioSampleRate(int rate) {
    if (rate <= 0) {
        return;
    }
    audioRate = rate;
    audioSamplesPerScanline = static_cast<double>(audioRate) /
                              (getFrameRate() * static_cast<double>(getScanlinesPerFrame()));
    ymTicksPerOutputSample = (static_cast<double>(masterClockHz()) / M68K_DIVIDER / 144.0) /
                             static_cast<double>(audioRate);
    audioSampleCounter = 0.0;
    ymOutputTickCounter = 0.0;
    clearYMSampleQueue();
}

void Genesis::clearYMSampleQueue() {
    ymNativeQueueHead = 0;
    ymNativeQueueTail = 0;
    ymNativeQueueCount = 0;
    ymLastSample = {0, 0};
}

void Genesis::pushYMSample(const YMSample& sample) {
    if (ymNativeQueueCount >= YM_NATIVE_QUEUE_SIZE) {
        ymNativeQueueHead = (ymNativeQueueHead + 1) % YM_NATIVE_QUEUE_SIZE;
        ymNativeQueueCount--;
    }
    ymNativeQueue[ymNativeQueueTail] = sample;
    ymNativeQueueTail = (ymNativeQueueTail + 1) % YM_NATIVE_QUEUE_SIZE;
    ymNativeQueueCount++;
}

bool Genesis::popYMSample(YMSample& outSample) {
    if (ymNativeQueueCount <= 0) {
        return false;
    }
    outSample = ymNativeQueue[ymNativeQueueHead];
    ymNativeQueueHead = (ymNativeQueueHead + 1) % YM_NATIVE_QUEUE_SIZE;
    ymNativeQueueCount--;
    ymLastSample = outSample;
    return true;
}

void Genesis::clockYM(int m68kCycles) {
    // Low-pass filter coefficients: first-order Butterworth at ~3390 Hz
    // Matches Model 1 VA0-VA2 analog RC filter at YM2612 native rate (~53267 Hz)
    constexpr double b0 = 0.1684983368367697;
    constexpr double b1 = 0.1684983368367697;
    constexpr double a1 = -0.6630033263264605;

    ymNativeTickAccum += m68kCycles;
    int ymTicks = ymNativeTickAccum / 144;
    ymNativeTickAccum %= 144;
    for (int i = 0; i < ymTicks; i++) {
        YMSample sample = ym2612.tick();

        double outL = b0 * sample.left + b1 * ymLpfPrevIn[0] - a1 * ymLpfPrevOut[0];
        ymLpfPrevIn[0] = sample.left;
        ymLpfPrevOut[0] = outL;
        sample.left = static_cast<s32>(outL);

        double outR = b0 * sample.right + b1 * ymLpfPrevIn[1] - a1 * ymLpfPrevOut[1];
        ymLpfPrevIn[1] = sample.right;
        ymLpfPrevOut[1] = outR;
        sample.right = static_cast<s32>(outR);

        pushYMSample(sample);
    }
}

void Genesis::clockZ80(int m68kCycles) {
    // Convert elapsed 68K time into Z80 cycles and carry the budget across
    // calls. Instruction overshoot is kept as negative debt so it is paid back
    // by later elapsed time instead of becoming free extra Z80 execution.
    z80CycleAccum += m68kCycles * M68K_DIVIDER;
    int z80Cycles = z80CycleAccum / Z80_DIVIDER;
    z80CycleAccum %= Z80_DIVIDER;
    z80CycleDebt += z80Cycles;

    if (bus.z80Reset || bus.z80BusRequested) {
        if (z80CycleDebt > 0) {
            z80CycleDebt = 0;
        }
        return;
    }

    while (z80CycleDebt > 0) {
        int ran = z80.execute();
        if (ran <= 0) {
            break;
        }
        z80CycleDebt -= ran;
    }
}

void Genesis::setVideoStandard(VideoStandard standard) {
    vdp.setVideoStandard(standard);
    bus.setVideoStandard(standard);
    setAudioSampleRate(audioRate);
}

bool Genesis::loadROM(const char* filename) {
    if (!cartridge.load(filename)) {
        return false;
    }
    
    // Update bus with cartridge
    // For now, bus reads directly from cartridge through its own ROM loading
    // This is a simplified approach - in a full implementation, bus would reference cartridge
    
    // Copy ROM data to bus (temporary approach)
    bus.loadROM(filename);
    
    reset();
    return true;
}

void Genesis::reset() {
    m68k.reset();
    z80.reset();
    vdp.reset();
    bus.reset();
    ym2612.reset();
    psg.reset();
    
    frameCount = 0;
    paused = false;
    frameProfile_ = {};
    audioBufferPos = 0;
    audioSampleCounter = 0.0;
    ymOutputTickCounter = 0.0;
    z80CycleAccum = 0;
    ymNativeTickAccum = 0;
    m68kCycleDebt = 0;
    z80CycleDebt = 0;
    m68kLineAccum = 0;

    m68k.refreshCounter_ = 0;
    m68k.refreshLastSync_ = m68k.masterCycles_;

    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    clearYMSampleQueue();

    std::memset(controllerData, 0x7F, sizeof(controllerData));
    std::memset(controllerCtrl, 0, sizeof(controllerCtrl));
    std::memset(dcBlockPrevIn, 0, sizeof(dcBlockPrevIn));
    std::memset(dcBlockPrevOut, 0, sizeof(dcBlockPrevOut));
    std::memset(ymLpfPrevIn, 0, sizeof(ymLpfPrevIn));
    std::memset(ymLpfPrevOut, 0, sizeof(ymLpfPrevOut));
    resetBadSplitTraceState();

    printf("Genesis reset complete. Starting execution at PC=%08X\n", m68k.getPC());
}

void Genesis::resetBadSplitTraceState() {
    std::memset(badSplitTraceRing_, 0, sizeof(badSplitTraceRing_));
    badSplitTraceWriteIndex_ = 0;
    badSplitTraceCount_ = 0;
    badSplitTraceSeenSerial_ = vdp.getDisplayEnableApplySerial();
    badSplitTraceDumped_ = false;
    badSplitTraceActive_ = false;
    badSplitTraceComplete_ = false;
    badSplitTraceSawNormalSplit_ = false;
}

void Genesis::appendBadSplitTraceEntry(int scanline, int lineCycle, int cycles) {
    BadSplitTraceEntry& entry = badSplitTraceRing_[badSplitTraceWriteIndex_];
    entry.frame = frameCount;
    entry.scanline = scanline;
    entry.lineCycle = lineCycle;
    entry.cycles = cycles;
    entry.pendingInterrupt = m68k.state.pendingInterrupt;
    entry.pc = m68k.getLastPC();
    entry.nextPc = m68k.getPC();
    entry.ir = m68k.getLastIR();
    entry.sr = m68k.getSR();

    badSplitTraceWriteIndex_ = (badSplitTraceWriteIndex_ + 1) % BAD_SPLIT_TRACE_RING_SIZE;
    if (badSplitTraceCount_ < BAD_SPLIT_TRACE_RING_SIZE) {
        badSplitTraceCount_++;
    }
}

void Genesis::dumpBadSplitTraceHistory(int disableLine) const {
    std::fprintf(stderr,
                 "[BAD-SPLIT] arm frame=%d disableLn=%d entries=%d\n",
                 frameCount,
                 disableLine,
                 badSplitTraceCount_);

    int index = badSplitTraceWriteIndex_ - badSplitTraceCount_;
    if (index < 0) {
        index += BAD_SPLIT_TRACE_RING_SIZE;
    }

    int cumulativeCycles = 0;
    for (int i = 0; i < badSplitTraceCount_; i++) {
        const BadSplitTraceEntry& entry = badSplitTraceRing_[(index + i) % BAD_SPLIT_TRACE_RING_SIZE];
        cumulativeCycles += entry.cycles;
        const char* label = badSplitTraceLabel(entry.pc);
        std::fprintf(stderr,
                     "[BAD-SPLIT-HIST] frame=%d ln=%d cyc=%d pc=%06X next=%06X ir=%04X ran=%d sum=%d sr=%04X pendInt=%d label=%s\n",
                     entry.frame,
                     entry.scanline,
                     entry.lineCycle,
                     entry.pc,
                     entry.nextPc,
                     entry.ir,
                     entry.cycles,
                     cumulativeCycles,
                     entry.sr,
                     entry.pendingInterrupt,
                     label);
    }
}

void Genesis::checkBadSplitTraceEvents() {
    static const bool logBadSplitTrace = envEnabled("GENESIS_LOG_BAD_SPLIT_TRACE");
    static const int badSplitTraceTargetLine = []() {
        const char* env = std::getenv("GENESIS_LOG_BAD_SPLIT_TRACE_LINE");
        return env ? std::atoi(env) : 0;
    }();
    if (!logBadSplitTrace || badSplitTraceComplete_) {
        return;
    }

    const u32 serial = vdp.getDisplayEnableApplySerial();
    if (serial == badSplitTraceSeenSerial_) {
        return;
    }
    badSplitTraceSeenSerial_ = serial;

    const int applyLine = vdp.getLastDisplayEnableApplyScanline();
    const int applyPixel = vdp.getLastDisplayEnableApplyPixel();
    const bool applyValue = vdp.getLastDisplayEnableApplyValue();

    if (!applyValue && applyLine == 109 && applyPixel == 0) {
        badSplitTraceSawNormalSplit_ = true;
    }

    bool isRealLateSplit = (applyLine == 110) || (applyLine >= 119 && applyLine < vdp.activeHeight);
    if (badSplitTraceTargetLine > 0) {
        isRealLateSplit = (applyLine == badSplitTraceTargetLine);
    }
    if (!badSplitTraceActive_ && !badSplitTraceDumped_ &&
        badSplitTraceSawNormalSplit_ &&
        applyPixel == 0 && !applyValue && isRealLateSplit) {
        dumpBadSplitTraceHistory(applyLine);
        badSplitTraceDumped_ = true;
        badSplitTraceActive_ = true;
        std::fprintf(stderr,
                     "[BAD-SPLIT-EVENT] frame=%d ln=%d px=%d val=%d phase=disable\n",
                     frameCount,
                     applyLine,
                     applyPixel,
                     applyValue ? 1 : 0);
        return;
    }

    if (!badSplitTraceActive_) {
        return;
    }

    std::fprintf(stderr,
                 "[BAD-SPLIT-EVENT] frame=%d ln=%d px=%d val=%d phase=%s\n",
                 frameCount,
                 applyLine,
                 applyPixel,
                 applyValue ? 1 : 0,
                 applyValue ? "enable" : "disable");
    if (applyLine < vdp.activeHeight && applyPixel == 0 && applyValue) {
        badSplitTraceActive_ = false;
        badSplitTraceComplete_ = true;
        std::fprintf(stderr,
                     "[BAD-SPLIT] complete frame=%d enableLn=%d\n",
                     frameCount,
                     applyLine);
    }
}

void Genesis::runFrame() {
    frameProfile_ = {};
    if (paused) return;

    {
        static const bool logCycles = []() {
            const char* e = std::getenv("GENESIS_LOG_FRAME_CYCLES");
            return e && e[0] != '0';
        }();
        static u64 prevCycles = 0;
        if (logCycles) {
            u64 cur = m68k.totalCycles_;
            std::fprintf(stderr, "[FCYC] frame=%d cycles=%llu delta=%llu\n",
                         frameCount + 1, static_cast<unsigned long long>(cur),
                         static_cast<unsigned long long>(cur - prevCycles));
            prevCycles = cur;
        }
    }


    audioBufferPos = 0;
    vdp.debugFrameNumber_ = frameCount + 1;
    m68k.traceFrameNumber_ = frameCount + 1;

    for (int line = 0; line < getScanlinesPerFrame(); line++) {
        m68k.traceScanline_ = line;
        bus.tickControllerProtocol();
        runScanline();
    }

    frameCount++;

    static const bool logFrames = g_debugMode && envEnabled("GENESIS_LOG_FRAMES");
    if (logFrames && (frameCount <= 5 || frameCount % 300 == 0)) {
        printf("Frame %d: PC=%08X SR=%04X (mask=%d) D0=%08X A7=%08X\n",
               frameCount, m68k.getPC(), m68k.getSR(),
               (m68k.getSR() >> 8) & 7,
               m68k.state.d[0], m68k.state.a[7]);
    }

}

void Genesis::runScanline() {
    const bool profile = detailedProfilingEnabled_;
    const int currentFrame = frameCount + 1;
    static const bool logHintTiming = g_debugMode && envEnabled("GENESIS_LOG_HINT_TIMING");
    static const bool logDividerM68K = g_debugMode && envEnabled("GENESIS_LOG_M68K_DIVIDER");
    static const bool logDividerRegs = g_debugMode && envEnabled("GENESIS_LOG_M68K_DIVIDER_REGS");
    static const bool logSplitWork = g_debugMode && envEnabled("GENESIS_LOG_SPLIT_WORK");
    static const bool logVintWork = g_debugMode && envEnabled("GENESIS_LOG_VINT_WORK");
    static const bool logHintHandler = g_debugMode && envEnabled("GENESIS_LOG_HINT_HANDLER");
    static const bool logBadSplitTrace = envEnabled("GENESIS_LOG_BAD_SPLIT_TRACE");

    if (profile) {
        auto start = ProfileClock::now();
        cheatEngine_->applyEnabledCheats();
        frameProfile_.cheatsMs += elapsedMs(start);
    } else {
        cheatEngine_->applyEnabledCheats();
    }

    // Use exact master clock: 3420 master cycles / 7 = 488.57 M68K cycles per
    // scanline. The accumulator distributes the remainder so no cycles are lost.
    m68kLineAccum += MASTER_CYCLES_PER_SCANLINE;
    int m68kThisLine = m68kLineAccum / M68K_DIVIDER;
    m68kLineAccum %= M68K_DIVIDER;
    vdp.lineCycleBudget = m68kThisLine;

    // Render the current line, but keep the current scanline visible until
    // the line budget is fully consumed.
    if (profile) {
        auto start = ProfileClock::now();
        vdp.beginScanline();
        frameProfile_.vdpMs += elapsedMs(start);
    } else {
        vdp.beginScanline();
    }
    logMegaTimingState("CPU-LINE-START", currentFrame, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus);

    if (logBadSplitTrace) {
        if (!badSplitTraceActive_ && !badSplitTraceComplete_ && vdp.getScanline() == 210) {
            badSplitTraceWriteIndex_ = 0;
            badSplitTraceCount_ = 0;
        }
        checkBadSplitTraceEvents();
    }

    // If the previous scanline ended mid-instruction, carry that overshoot
    // into this line's VDP phase so CPU/VDP timing stays continuous across
    // scanline boundaries.
    int carryInCycles = 0;
    if (m68kCycleDebt < 0) {
        carryInCycles = -m68kCycleDebt;
        if (carryInCycles > m68kThisLine) {
            carryInCycles = m68kThisLine;
        }
    }
    if (carryInCycles > 0) {
        if (profile) {
            auto vdpStart = ProfileClock::now();
            vdp.clockM68K(carryInCycles);
            frameProfile_.vdpMs += elapsedMs(vdpStart);
        } else {
            vdp.clockM68K(carryInCycles);
        }
        // Drain any FIFO entries enqueued during carry-in (the previous
        // line's H-int handler may have written VSRAM while straddling
        // the line boundary). Then re-latch vsramSnapshot.
        vdp.drainPendingFIFOForSnapshot();
        vdp.relatchVsramSnapshot();
    }

    // Deliver pending interrupts before the 68K runs.
    bool z80VIntDelivered = false;
    // V-int has priority: assert level 6 to both Z80 and 68K.
    // H-int is only delivered when V-int is not pending (lower priority).
    if (vdp.vblankPending() && vdp.getScanline() == vdp.activeHeight) {
        logMegaTimingState("IRQ-PRE-VINT", currentFrame, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus, 6);
        z80.interrupt();
        z80VIntDelivered = true;
        m68k.interrupt(6);
    }
    if (!vdp.vblankPending() && vdp.hblankIRQAsserted()) {
        logMegaTimingState("IRQ-PRE-HINT", currentFrame, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus, 4);
        if (logHintTiming) {
            std::fprintf(stderr, "[PRELOOP-HINT] ln=%d lineCyc=%d mask=%d pendInt=%d\n",
                         vdp.getScanline(),
                         vdp.lineCycles,
                         (m68k.getSR() >> 8) & 7,
                         m68k.state.pendingInterrupt);
        }
        m68k.interrupt(4);
    }

    // Run M68K and Z80 interleaved for proper timing
    // Carry over deficit from previous scanline so overshoot isn't "free"
    int m68kCyclesRemaining = m68kThisLine + m68kCycleDebt;
    auto deliverPendingInterrupts = [&]() {
        // Deliver newly-raised interrupts (H-int fires mid-line at HBlank).
        // Model the 68K's level-sensitive IPL pins: always reflect the VDP's
        // current interrupt state. When a status-register read clears the
        // pending flags, de-assert the corresponding CPU pending level so a
        // stale H-int doesn't fire after a VBlank handler reads status.
        if (vdp.vblankPending()) {
            m68k.applyRefresh();
            if (!z80VIntDelivered && vdp.getScanline() == vdp.activeHeight) {
                z80.interrupt();
                z80VIntDelivered = true;
            }
            logMegaTimingState("IRQ-LOOP-VINT", currentFrame, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus, 6);
            m68k.interrupt(6);
        } else if (vdp.hblankIRQAsserted()) {
            m68k.applyRefresh();
            logMegaTimingState("IRQ-LOOP-HINT", currentFrame, vdp.getScanline(), vdp.activeHeight, vdp.lineCycles, vdp, m68k, bus, 4);
            if (logHintTiming) {
                std::fprintf(stderr, "[INLOOP-HINT] ln=%d lineCyc=%d mask=%d pendInt=%d budgetLeft=%d\n",
                             vdp.getScanline(),
                             vdp.lineCycles,
                             (m68k.getSR() >> 8) & 7,
                             m68k.state.pendingInterrupt,
                             m68kCyclesRemaining);
            }
            m68k.interrupt(4);
        } else if (m68k.state.pendingInterrupt == 4 || m68k.state.pendingInterrupt == 6) {
            m68k.state.pendingInterrupt = -1;
        }
    };

    while (m68kCyclesRemaining > 0) {
        // 68K→VDP DMA (modes 0/1) halts the 68K bus.
        // Skip directly to the next external slot instead of advancing 1 cycle at a time.
        if (vdp.is68kDMABusy()) {
            int skip = vdp.dmaWaitCycles();
            if (skip <= 0) skip = 1;
            if (skip > m68kCyclesRemaining) skip = m68kCyclesRemaining;
            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.clockM68K(skip);
                frameProfile_.vdpMs += elapsedMs(vdpStart);

                auto psgStart = ProfileClock::now();
                psg.clock(skip);
                frameProfile_.psgMs += elapsedMs(psgStart);
            } else {
                vdp.clockM68K(skip);
                psg.clock(skip);
            }
            if (profile) {
                auto ymStart = ProfileClock::now();
                clockYM(skip);
                frameProfile_.ymMs += elapsedMs(ymStart);
            } else {
                clockYM(skip);
            }
            if (profile) {
                auto z80Start = ProfileClock::now();
                clockZ80(skip);
                frameProfile_.z80Ms += elapsedMs(z80Start);
            } else {
                clockZ80(skip);
            }
            m68k.advanceRefreshNoWait(skip);
            m68kCyclesRemaining -= skip;
            continue;
        }

        // Stall 68K while VDP FIFO is full (real hardware holds /DTACK).
        // Skip directly to the next external slot where a FIFO entry can drain.
        while (vdp.isVDPFIFOFull() && m68kCyclesRemaining > 0) {
            int skip = vdp.fifoWaitCycles();
            if (skip <= 0) skip = 1;
            if (skip > m68kCyclesRemaining) skip = m68kCyclesRemaining;
            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.clockM68K(skip);
                frameProfile_.vdpMs += elapsedMs(vdpStart);

                auto psgStart = ProfileClock::now();
                psg.clock(skip);
                frameProfile_.psgMs += elapsedMs(psgStart);
            } else {
                vdp.clockM68K(skip);
                psg.clock(skip);
            }
            if (profile) {
                auto ymStart = ProfileClock::now();
                clockYM(skip);
                frameProfile_.ymMs += elapsedMs(ymStart);
            } else {
                clockYM(skip);
            }
            // Z80 runs independently of 68K FIFO stalls
            if (profile) {
                auto z80Start = ProfileClock::now();
                clockZ80(skip);
                frameProfile_.z80Ms += elapsedMs(z80Start);
            } else {
                clockZ80(skip);
            }
            m68k.advanceRefreshNoWait(skip);
            m68kCyclesRemaining -= skip;
        }
        if (m68kCyclesRemaining <= 0) break;

        const int burstStartLineCycle = vdp.lineCycles;
        int burstTargetCycles = m68kCyclesRemaining;
        const int nextEventLineCycle = vdp.nextCpuEventLineCycle();
        if (nextEventLineCycle > burstStartLineCycle) {
            int cyclesToEvent = nextEventLineCycle - burstStartLineCycle;
            if (cyclesToEvent > 0 && cyclesToEvent < burstTargetCycles) {
                burstTargetCycles = cyclesToEvent;
            }
        }
        if (burstTargetCycles <= 0) {
            burstTargetCycles = 1;
        }

        vdp.setCpuTimingBaseLineCycle(burstStartLineCycle);
        int burstCycles = 0;
        while (burstCycles < burstTargetCycles) {
            int cycles = 0;
            m68k.setMemoryTimingOffset(burstCycles);
            if (profile) {
                auto m68kStart = ProfileClock::now();
                cycles = m68k.execute();
                frameProfile_.m68kMs += elapsedMs(m68kStart);
            } else {
                cycles = m68k.execute();
            }
            if (cycles <= 0) {
                burstCycles = 0;
                break;
            }

            if (logBadSplitTrace &&
                !badSplitTraceComplete_ &&
                (badSplitTraceActive_ || shouldTrackBadSplitTraceLine(vdp.getScanline()))) {
                appendBadSplitTraceEntry(vdp.getScanline(), vdp.lineCycles, cycles);
                if (badSplitTraceActive_) {
                    const char* label = badSplitTraceLabel(m68k.getLastPC());
                    std::fprintf(stderr,
                                 "[BAD-SPLIT-LIVE] frame=%d ln=%d cyc=%d pc=%06X next=%06X ir=%04X ran=%d sr=%04X pendInt=%d label=%s\n",
                                 frameCount,
                                 vdp.getScanline(),
                                 vdp.lineCycles,
                                 m68k.getLastPC(),
                                 m68k.getPC(),
                                 m68k.getLastIR(),
                                 cycles,
                                 m68k.getSR(),
                                 m68k.state.pendingInterrupt,
                                 label);
                }
            }

            if (logDividerM68K) {
                const int line = vdp.getScanline();
                const int mask = (m68k.getSR() >> 8) & 7;
                const bool dividerWindow = (line >= 100 && line <= 125) || (line >= 210 && line <= 225);
                if (dividerWindow && (mask >= 4 || vdp.hblankPending() || m68k.state.pendingInterrupt >= 0)) {
                    std::fprintf(stderr,
                                 "[M68K] ln=%d cyc=%d pc=%06X ir=%04X ran=%d mask=%d pendInt=%d hpend=%d\n",
                                 line,
                                 vdp.lineCycles,
                                 m68k.getLastPC(),
                                 m68k.getLastIR(),
                                 cycles,
                                 mask,
                                 m68k.state.pendingInterrupt,
                                 vdp.hblankPending() ? 1 : 0);
                }
                if (logDividerRegs) {
                    const u32 pc = m68k.getLastPC();
                    if (pc >= 0x00E066 && pc <= 0x00E144) {
                        std::fprintf(stderr,
                                     "[M68K-REGS] ln=%d cyc=%d pc=%06X d0=%08X d1=%08X d3=%08X d5=%08X d6=%08X a0=%08X a1=%08X a6=%08X sr=%04X pendInt=%d\n",
                                     line,
                                     vdp.lineCycles,
                                     pc,
                                     m68k.state.d[0],
                                     m68k.state.d[1],
                                     m68k.state.d[3],
                                     m68k.state.d[5],
                                     m68k.state.d[6],
                                     m68k.state.a[0],
                                     m68k.state.a[1],
                                     m68k.state.a[6],
                                     m68k.getSR(),
                                     m68k.state.pendingInterrupt);
                    }
                }
                if (logSplitWork) {
                    const u32 pc = m68k.getLastPC();
                    if ((pc >= 0x00E330 && pc <= 0x00E35C) ||
                        (pc >= 0x00DF8A && pc <= 0x00E144)) {
                        std::fprintf(stderr,
                                     "[SPLIT-WORK] frame=%d ln=%d cyc=%d pc=%06X ir=%04X ran=%d sr=%04X d0=%08X d1=%08X d3=%08X d4=%08X d5=%08X d6=%08X a0=%08X a1=%08X a6=%08X pendInt=%d hpend=%d\n",
                                     frameCount,
                                     line,
                                     vdp.lineCycles,
                                     pc,
                                     m68k.getLastIR(),
                                     cycles,
                                     m68k.getSR(),
                                     m68k.state.d[0],
                                     m68k.state.d[1],
                                     m68k.state.d[3],
                                     m68k.state.d[4],
                                     m68k.state.d[5],
                                     m68k.state.d[6],
                                     m68k.state.a[0],
                                     m68k.state.a[1],
                                     m68k.state.a[6],
                                     m68k.state.pendingInterrupt,
                                     vdp.hblankPending() ? 1 : 0);
                    }
                }
                if (logVintWork) {
                    const u32 pc = m68k.getLastPC();
                    if ((pc >= 0x000408 && pc <= 0x00071E) ||
                        (pc >= 0x00111C && pc <= 0x001156) ||
                        (pc >= 0x001528 && pc <= 0x001778)) {
                        std::fprintf(stderr,
                                     "[VINT-WORK] frame=%d ln=%d cyc=%d pc=%06X ir=%04X ran=%d sr=%04X d0=%08X d1=%08X d3=%08X d4=%08X d5=%08X d6=%08X a0=%08X a1=%08X a3=%08X a6=%08X pendInt=%d hpend=%d\n",
                                     frameCount,
                                     line,
                                     vdp.lineCycles,
                                     pc,
                                     m68k.getLastIR(),
                                     cycles,
                                     m68k.getSR(),
                                     m68k.state.d[0],
                                     m68k.state.d[1],
                                     m68k.state.d[3],
                                     m68k.state.d[4],
                                     m68k.state.d[5],
                                     m68k.state.d[6],
                                     m68k.state.a[0],
                                     m68k.state.a[1],
                                     m68k.state.a[3],
                                     m68k.state.a[6],
                                     m68k.state.pendingInterrupt,
                                     vdp.hblankPending() ? 1 : 0);
                    }
                }
            }
            if (logHintHandler) {
                const u32 pc = m68k.getLastPC();
                if (pc >= 0x000F54 && pc <= 0x00100A) {
                    std::fprintf(stderr,
                                 "[HHDL] ln=%d cyc=%d pc=%06X ir=%04X ran=%d mask=%d pendInt=%d hpend=%d vpend=%d\n",
                                 vdp.getScanline(),
                                 vdp.lineCycles,
                                 pc,
                                 m68k.getLastIR(),
                                 cycles,
                                 (m68k.getSR() >> 8) & 7,
                                 m68k.state.pendingInterrupt,
                                 vdp.hblankPending() ? 1 : 0,
                                 vdp.vblankPending() ? 1 : 0);
                }
            }

            burstCycles += cycles;

            // Sync VDP after every instruction so events (H-int, external
            // slot drains, V-int) fire at exact cycle boundaries rather
            // than being quantized to burst-end.
            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.syncToLineCycle(burstStartLineCycle + burstCycles);
                frameProfile_.vdpMs += elapsedMs(vdpStart);
            } else {
                vdp.syncToLineCycle(burstStartLineCycle + burstCycles);
            }


            // Deliver interrupts after each instruction's VDP sync so
            // H-int fires at a deterministic cycle, not at the next
            // burst boundary.
            deliverPendingInterrupts();

            if (burstCycles >= burstTargetCycles || burstCycles >= m68kCyclesRemaining ||
                vdp.is68kDMABusy() || vdp.isVDPFIFOFull()) {
                break;
            }
        }
        m68k.setMemoryTimingOffset(0);
        if (burstCycles <= 0) {
            break;
        }

        m68kCyclesRemaining -= burstCycles;

        if (logBadSplitTrace) {
            checkBadSplitTraceEvents();
        }

        // Run PSG in sync with M68K
        if (profile) {
            auto psgStart = ProfileClock::now();
            psg.clock(burstCycles);
            frameProfile_.psgMs += elapsedMs(psgStart);
        } else {
            psg.clock(burstCycles);
        }

        if (profile) {
            auto ymStart = ProfileClock::now();
            clockYM(burstCycles);
            frameProfile_.ymMs += elapsedMs(ymStart);
        } else {
            clockYM(burstCycles);
        }

        // Run Z80 proportionally (M68K:Z80 ratio = 7.67:3.58 MHz = 15:7)
        if (profile) {
            auto z80Start = ProfileClock::now();
            clockZ80(burstCycles);
            frameProfile_.z80Ms += elapsedMs(z80Start);
        } else {
            clockZ80(burstCycles);
        }


    }

    // Apply accumulated bus refresh penalties at the scanline boundary.
    // This matches BlastEm's DEFAULT_SYNC_INTERVAL = MCLKS_LINE.
    {
        int refreshPenalty = m68k.applyRefresh();
        m68kCyclesRemaining -= refreshPenalty;
    }

    // Save overshoot so next scanline starts with correct budget
    m68kCycleDebt = m68kCyclesRemaining;  // negative = overshoot

    // Generate audio samples for this scanline
    // Output rate uses the opened SDL device frequency.
    // YM2612 FM sample rate: M68K_CLOCK / 144 = MASTER / 7 / 144 ≈ 53,267 Hz
    audioSampleCounter += audioSamplesPerScanline;
    int samplesToGenerate = static_cast<int>(audioSampleCounter);
    audioSampleCounter -= samplesToGenerate;

    for (int i = 0; i < samplesToGenerate; i++) {
        if (audioBufferPos + 1 >= AUDIO_BUFFER_SIZE) break;

        ymOutputTickCounter += ymTicksPerOutputSample;
        int ymTicks = static_cast<int>(ymOutputTickCounter);
        ymOutputTickCounter -= ymTicks;

        s32 ymLSum = 0;
        s32 ymRSum = 0;
        int consumedTicks = 0;
        YMSample ymSample{};
        while (consumedTicks < ymTicks && popYMSample(ymSample)) {
            ymLSum += ymSample.left;
            ymRSum += ymSample.right;
            consumedTicks++;
        }

        s32 left = 0;
        s32 right = 0;
        if (consumedTicks > 0) {
            left = ymLSum / consumedTicks;
            right = ymRSum / consumedTicks;
        } else {
            left = ymLastSample.left;
            right = ymLastSample.right;
        }

        // Mix PSG (mono, add to both channels)
        // Model 1 VA7+/Model 2: PSG through 47-51kΩ, FM through 4.7kΩ (~-12 dB)
        s32 psgSample = 0;
        if (profile) {
            auto psgStart = ProfileClock::now();
            psgSample = psg.getSample();
            frameProfile_.psgMs += elapsedMs(psgStart);
        } else {
            psgSample = psg.getSample();
        }
        psgSample >>= 2;  // ~-12 dB to match hardware mixing levels (VA7+/Model 2)
        left += psgSample;
        right += psgSample;

        // DC-blocking filter: y[n] = x[n] - x[n-1] + α·y[n-1]
        // Emulates hardware AC coupling (output capacitor). Removes DC offset
        // from DAC-enabled-with-no-data and other constant offsets.
        // α ≈ 0.995 gives a ~15 Hz cutoff at 48 kHz, matching typical Genesis
        // output stage RC values.
        constexpr double dcAlpha = 0.995;
        double inL = static_cast<double>(left);
        double inR = static_cast<double>(right);
        double outL = inL - dcBlockPrevIn[0] + dcAlpha * dcBlockPrevOut[0];
        double outR = inR - dcBlockPrevIn[1] + dcAlpha * dcBlockPrevOut[1];
        dcBlockPrevIn[0] = inL;
        dcBlockPrevIn[1] = inR;
        dcBlockPrevOut[0] = outL;
        dcBlockPrevOut[1] = outR;
        left = static_cast<s32>(outL);
        right = static_cast<s32>(outR);

        if (profile) {
            auto mixStart = ProfileClock::now();
            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;

            audioBuffer[audioBufferPos++] = static_cast<s16>(left);
            audioBuffer[audioBufferPos++] = static_cast<s16>(right);
            frameProfile_.mixMs += elapsedMs(mixStart);
        } else {
            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;

            audioBuffer[audioBufferPos++] = static_cast<s16>(left);
            audioBuffer[audioBufferPos++] = static_cast<s16>(right);
        }
    }

    // Advance to the next scanline only after the CPUs have consumed this
    // line's timing budget.
    if (profile) {
        auto vdpStart = ProfileClock::now();
        vdp.endScanline();
        frameProfile_.vdpMs += elapsedMs(vdpStart);
    } else {
        vdp.endScanline();
    }
}

void Genesis::step() {
    m68k.execute();
}

bool Genesis::canonicalizeLoadedBoundaryState() {
    if (vdp.lineCycleBudget <= 0 || vdp.lineCycles < vdp.lineCycleBudget) {
        return false;
    }

    vdp.lineCycles = 0;
    vdp.currentSlotIndex = -1;
    vdp.hblankCounterDecremented_ = false;
    vdp.vramWriteSecondSlot = false;
    vdp.dmaVramSecondSlot = false;
    vdp.dmaCopySecondSlot = false;
    vdp.renderedPixels = 0;
    vdp.cramDotCount = 0;
    vdp.lineIsBlank = (vdp.scanline >= vdp.activeHeight) || !vdp.displayEnabled;
    vdp.spriteLineCount[0] = 0;
    vdp.spriteLineCount[1] = 0;
    vdp.spriteLineValid[0] = false;
    vdp.spriteLineValid[1] = false;
    return true;
}

// Save state format: magic + version + components + thumbnail
static constexpr u32 SAVE_MAGIC = 0x47454E53; // "GENS"
static constexpr u32 SAVE_VERSION = 16;
static constexpr int THUMB_W = 320;
static constexpr int THUMB_H = 224;

namespace {
template<typename T>
void writeVal(std::ofstream& f, const T& val) {
    f.write(reinterpret_cast<const char*>(&val), sizeof(T));
}
template<typename T>
bool readVal(std::ifstream& f, T& val) {
    f.read(reinterpret_cast<char*>(&val), sizeof(T));
    return f.good();
}
template<typename T>
void writeArr(std::ofstream& f, const T* arr, size_t count) {
    f.write(reinterpret_cast<const char*>(arr), static_cast<std::streamsize>(sizeof(T) * count));
}
template<typename T>
bool readArr(std::ifstream& f, T* arr, size_t count) {
    f.read(reinterpret_cast<char*>(arr), static_cast<std::streamsize>(sizeof(T) * count));
    return f.good();
}
} // namespace

std::string Genesis::getStatePath(int slot) const {
    std::filesystem::path dir = getConfigDir() / "saves";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string romName = std::filesystem::path(currentRomPath_).stem().string();
    if (romName.empty()) romName = "unknown";
    return (dir / (romName + ".ss" + std::to_string(slot))).string();
}

bool Genesis::hasState(int slot) const {
    std::string path = getStatePath(slot);
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

int Genesis::findLatestStateSlot() const {
    int bestSlot = -1;
    std::filesystem::file_time_type bestTime{};
    std::error_code ec;
    for (int i = 0; i < 10; i++) {
        std::string path = getStatePath(i);
        if (!std::filesystem::exists(path, ec)) continue;
        auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) continue;
        if (bestSlot < 0 || mtime > bestTime) {
            bestSlot = i;
            bestTime = mtime;
        }
    }
    return bestSlot;
}

bool Genesis::saveState(int slot, const u32* screenshotBuffer) {
    std::string path = getStatePath(slot);
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // Header
    writeVal(f, SAVE_MAGIC);
    writeVal(f, SAVE_VERSION);

    // M68K state
    writeVal(f, m68k.state);
    writeVal(f, m68k.totalCycles_);
    writeVal(f, m68k.masterCycles_);
    writeVal(f, m68k.refreshCounter_);
    writeVal(f, m68k.refreshLastSync_);

    // Z80 state
    writeVal(f, z80.state);

    // VDP state - memory
    writeArr(f, vdp.vram, 0x10000);
    writeArr(f, vdp.cram, 64);
    writeArr(f, vdp.vsram, 40);
    writeArr(f, vdp.regs, 24);
    // VDP control/DMA state
    writeVal(f, vdp.controlPending);
    writeVal(f, vdp.controlLatch);
    writeVal(f, vdp.code);
    writeVal(f, vdp.address);
    writeVal(f, vdp.dmaActive);
    writeVal(f, vdp.dmaMode);
    { int placeholder = 0; writeVal(f, placeholder); } // was dmaBytesRemaining
    writeVal(f, vdp.dmaWordsRemaining);
    { double placeholder = 0.0; writeVal(f, placeholder); } // was dmaCycleAccumulator
    writeVal(f, vdp.dmaFillPending);
    writeVal(f, vdp.dmaFillValue);
    writeVal(f, vdp.dmaFillCode);
    writeArr(f, vdp.fifo, VDP::FIFO_SIZE);
    writeVal(f, vdp.fifoCount);
    writeVal(f, vdp.fifoReadIndex);
    writeVal(f, vdp.fifoWriteIndex);
    { int placeholder = 0; writeVal(f, placeholder); } // was fifoSlotCounter
    writeVal(f, vdp.oddFrame);
    writeVal(f, vdp.scanline);
    writeVal(f, vdp.hcounter);
    writeVal(f, vdp.lineCycles);
    writeVal(f, vdp.vblankIRQ);
    writeVal(f, vdp.vblankFlag);
    writeVal(f, vdp.hblankIRQ);
    writeVal(f, vdp.hblankCounter);
    writeVal(f, vdp.spriteCollision);
    writeVal(f, vdp.spriteOverflow);
    writeVal(f, vdp.hvLatch);
    writeVal(f, vdp.hvLatched);
    writeVal(f, vdp.displayEnabled);
    writeVal(f, vdp.displayEnabledLatch);
    writeArr(f, vdp.displayEnableEvents, VDP::DISPLAY_ENABLE_EVENT_QUEUE_SIZE);
    writeVal(f, vdp.displayEnableEventCount);
    writeVal(f, vdp.displayEnableEventReadIndex);
    writeVal(f, vdp.displayEnableEventWriteIndex);

    // Slot timing state (v8+)
    writeVal(f, vdp.currentSlotIndex);
    writeVal(f, vdp.lineIsBlank);
    writeVal(f, vdp.vramWriteSecondSlot);
    writeVal(f, vdp.dmaVramSecondSlot);
    writeVal(f, vdp.dmaCopySecondSlot);
    writeVal(f, vdp.dmaAddress);
    writeVal(f, vdp.lineCycleBudget);
    writeVal(f, vdp.hblankCounterDecremented_);
    writeVal(f, vdp.vblankIRQArmed);
    writeVal(f, vdp.vblankIRQAssertCycle);

    // Bus state
    writeArr(f, bus.ram, 0x10000);
    writeArr(f, bus.z80Ram, 0x2000);
    writeVal(f, bus.z80Bank);
    writeArr(f, bus.romBankRegs, 8);
    writeVal(f, bus.sramMapped);
    writeArr(f, bus.ioData, 3);
    writeArr(f, bus.ioCtrl, 3);
    writeArr(f, bus.padState, 2);
    writeArr(f, bus.padState6, 2);
    writeArr(f, bus.thCounter, 2);
    writeArr(f, bus.thTimeoutLines, 2);
    writeArr(f, bus.prevTH, 2);
    writeVal(f, bus.z80BusRequested);
    writeVal(f, bus.z80Reset);

    // YM2612 state
    writeArr(f, ym2612.channels, 6);
    writeArr(f, ym2612.addressLatch, 2);
    writeVal(f, ym2612.lfoFreq);
    writeVal(f, ym2612.lfoEnabled);
    writeVal(f, ym2612.dacData);
    writeVal(f, ym2612.dacEnabled);
    writeVal(f, ym2612.timerA);
    writeVal(f, ym2612.timerB);
    writeVal(f, ym2612.timerControl);
    writeVal(f, ym2612.timerAOverflow);
    writeVal(f, ym2612.timerBOverflow);
    writeVal(f, ym2612.timerACounter);
    writeVal(f, ym2612.timerBCounter);
    writeVal(f, ym2612.timerBSubCounter);
    writeVal(f, ym2612.busyCounter);
    writeVal(f, ym2612.egCounter);
    writeVal(f, ym2612.egSubCounter);
    writeVal(f, ym2612.ch3SpecialMode);
    writeVal(f, ym2612.csmMode);
    writeArr(f, ym2612.ch3Fnum, 3);
    writeArr(f, ym2612.ch3Block, 3);
    writeArr(f, ym2612.ch3Keycode, 3);
    writeArr(f, ym2612.ch3FreqHiLatch, 3);
    writeVal(f, ym2612.lfoCounter);
    writeVal(f, ym2612.lfoPeriod);
    writeVal(f, ym2612.lfoAM);
    writeVal(f, ym2612.lfoPM);

    // PSG state
    writeArr(f, psg.tone, 3);
    writeVal(f, psg.noise);
    writeVal(f, psg.lfsr);
    writeVal(f, psg.latchedChannel);
    writeVal(f, psg.latchedVolume);
    writeVal(f, psg.clockCounter);
    writeVal(f, psg.sampleAccum);
    writeVal(f, psg.sampleCount);

    // Genesis internal state
    writeVal(f, frameCount);
    writeVal(f, audioSampleCounter);
    writeVal(f, ymOutputTickCounter);
    writeVal(f, z80CycleAccum);
    writeVal(f, ymNativeTickAccum);
    writeVal(f, m68kCycleDebt);
    writeVal(f, z80CycleDebt);
    writeVal(f, m68kLineAccum);
    writeArr(f, dcBlockPrevIn, 2);
    writeArr(f, dcBlockPrevOut, 2);
    writeArr(f, ymLpfPrevIn, 2);
    writeArr(f, ymLpfPrevOut, 2);
    writeArr(f, ymNativeQueue, YM_NATIVE_QUEUE_SIZE);
    writeVal(f, ymNativeQueueHead);
    writeVal(f, ymNativeQueueTail);
    writeVal(f, ymNativeQueueCount);
    writeVal(f, ymLastSample);
    writeVal(f, vdp.videoStandard_);

    // Thumbnail (320x224 u32 pixels)
    if (screenshotBuffer) {
        writeArr(f, screenshotBuffer, THUMB_W * THUMB_H);
    } else {
        writeArr(f, vdp.framebuffer, THUMB_W * THUMB_H);
    }

    return f.good();
}

bool Genesis::loadState(int slot) {
    return loadStateFromFile(getStatePath(slot));
}

bool Genesis::loadStateFromFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // Verify header
    u32 magic = 0, version = 0;
    readVal(f, magic);
    readVal(f, version);
    if (magic != SAVE_MAGIC || version < 1 || version > SAVE_VERSION) {
        return false;
    }

    // M68K state
    readVal(f, m68k.state);
    if (version >= 14) {
        readVal(f, m68k.totalCycles_);
    } else {
        m68k.totalCycles_ = 0;
    }
    if (version >= 16) {
        readVal(f, m68k.masterCycles_);
        readVal(f, m68k.refreshCounter_);
        readVal(f, m68k.refreshLastSync_);
    } else {
        m68k.masterCycles_ = m68k.totalCycles_ * 7;
        m68k.refreshCounter_ = 0;
        m68k.refreshLastSync_ = m68k.masterCycles_;
    }
    m68k.invalidatePrefetch();

    // Z80 state
    readVal(f, z80.state);
    if (version < 15) {
        z80.state.afterEi = false;
    }

    // VDP state
    readArr(f, vdp.vram, 0x10000);
    readArr(f, vdp.cram, 64);
    readArr(f, vdp.vsram, 40);
    readArr(f, vdp.regs, 24);
    readVal(f, vdp.controlPending);
    readVal(f, vdp.controlLatch);
    readVal(f, vdp.code);
    readVal(f, vdp.address);
    readVal(f, vdp.dmaActive);
    readVal(f, vdp.dmaMode);
    { int discard = 0; readVal(f, discard); } // was dmaBytesRemaining
    readVal(f, vdp.dmaWordsRemaining);
    { double discard = 0.0; readVal(f, discard); } // was dmaCycleAccumulator
    readVal(f, vdp.dmaFillPending);
    if (version >= 10) {
        readVal(f, vdp.dmaFillValue);
        readVal(f, vdp.dmaFillCode);
    } else {
        u8 oldFillValue = 0;
        readVal(f, oldFillValue);
        vdp.dmaFillValue = static_cast<u16>(oldFillValue) << 8;
        vdp.dmaFillCode = 1; // Default to VRAM target for old states
    }
    if (version >= 3) {
        readArr(f, vdp.fifo, VDP::FIFO_SIZE);
    } else {
        u16 oldFifo[VDP::FIFO_SIZE];
        readArr(f, oldFifo, VDP::FIFO_SIZE);
        for (int i = 0; i < VDP::FIFO_SIZE; i++) {
            vdp.fifo[i] = {};
        }
    }
    readVal(f, vdp.fifoCount);
    if (version >= 3) {
        readVal(f, vdp.fifoReadIndex);
        readVal(f, vdp.fifoWriteIndex);
    } else {
        vdp.fifoReadIndex = 0;
        vdp.fifoWriteIndex = 0;
        vdp.fifoCount = 0;
    }
    { int discard = 0; readVal(f, discard); } // was fifoSlotCounter
    readVal(f, vdp.oddFrame);
    readVal(f, vdp.scanline);
    readVal(f, vdp.hcounter);
    readVal(f, vdp.lineCycles);
    readVal(f, vdp.vblankIRQ);
    if (version >= 11) {
        readVal(f, vdp.vblankFlag);
    } else {
        vdp.vblankFlag = vdp.vblankIRQ;  // Approximate from old state
    }
    readVal(f, vdp.hblankIRQ);
    readVal(f, vdp.hblankCounter);
    readVal(f, vdp.spriteCollision);
    readVal(f, vdp.spriteOverflow);
    readVal(f, vdp.hvLatch);
    readVal(f, vdp.hvLatched);
    bool savedDisplayEnabled = false;
    bool savedDisplayEnabledLatch = false;
    VDPDisplayEnableEvent savedDisplayEnableEvents[VDP::DISPLAY_ENABLE_EVENT_QUEUE_SIZE] = {};
    int savedDisplayEnableEventCount = 0;
    int savedDisplayEnableEventReadIndex = 0;
    int savedDisplayEnableEventWriteIndex = 0;
    if (version >= 4) {
        readVal(f, savedDisplayEnabled);
        readVal(f, savedDisplayEnabledLatch);
        if (version >= 5) {
            readArr(f, savedDisplayEnableEvents, VDP::DISPLAY_ENABLE_EVENT_QUEUE_SIZE);
            readVal(f, savedDisplayEnableEventCount);
            readVal(f, savedDisplayEnableEventReadIndex);
            readVal(f, savedDisplayEnableEventWriteIndex);
        } else {
            int oldDisplayEnablePendingLine = -1;
            readVal(f, oldDisplayEnablePendingLine);
        }
    }
    // Rebuild decoded register values
    vdp.updateRegisters();
    vdp.refreshCachedColors();
    if (version >= 4) {
        vdp.displayEnabled = savedDisplayEnabled;
        vdp.displayEnabledLatch = savedDisplayEnabledLatch;
        if (version >= 5) {
            std::memcpy(vdp.displayEnableEvents,
                        savedDisplayEnableEvents,
                        sizeof(savedDisplayEnableEvents));
            vdp.displayEnableEventCount = savedDisplayEnableEventCount;
            vdp.displayEnableEventReadIndex = savedDisplayEnableEventReadIndex;
            vdp.displayEnableEventWriteIndex = savedDisplayEnableEventWriteIndex;
        } else {
            std::memset(vdp.displayEnableEvents, 0, sizeof(vdp.displayEnableEvents));
            vdp.displayEnableEventCount = 0;
            vdp.displayEnableEventReadIndex = 0;
            vdp.displayEnableEventWriteIndex = 0;
        }
    } else {
        vdp.displayEnabled = vdp.displayEnabledLatch;
        std::memset(vdp.displayEnableEvents, 0, sizeof(vdp.displayEnableEvents));
        vdp.displayEnableEventCount = 0;
        vdp.displayEnableEventReadIndex = 0;
        vdp.displayEnableEventWriteIndex = 0;
    }
    vdp.interlaceMode = vdp.interlaceModeLatch;

    int loadedLineCycleBudget = VDP_MAX_M68K_CYCLES;
    bool loadedHblankCounterDecremented = false;
    bool loadedVblankIRQArmed = false;
    int loadedVblankIRQAssertCycle = 0;

    // Slot timing state (v8+)
    if (version >= 8) {
        readVal(f, vdp.currentSlotIndex);
        readVal(f, vdp.lineIsBlank);
        readVal(f, vdp.vramWriteSecondSlot);
        readVal(f, vdp.dmaVramSecondSlot);
        readVal(f, vdp.dmaCopySecondSlot);
        if (version >= 9) {
            readVal(f, vdp.dmaAddress);
        } else {
            vdp.dmaAddress = vdp.address;
        }
        if (version >= 13) {
            readVal(f, loadedLineCycleBudget);
            readVal(f, loadedHblankCounterDecremented);
            readVal(f, loadedVblankIRQArmed);
            readVal(f, loadedVblankIRQAssertCycle);
        }
    } else {
        // Reconstruct slot state from lineCycles
        vdp.lineIsBlank = (vdp.scanline >= vdp.activeHeight);
        const VDPSlotTable& table = (vdp.activeWidth == 320)
            ? (vdp.lineIsBlank ? kH40Blank : kH40Active)
            : (vdp.lineIsBlank ? kH32Blank : kH32Active);
        int cyc = vdp.lineCycles;
        if (cyc < 0) cyc = 0;
        if (cyc >= VDP_MAX_M68K_CYCLES) cyc = VDP_MAX_M68K_CYCLES - 1;
        vdp.currentSlotIndex = table.slotAtM68kCycle[cyc];
        vdp.vramWriteSecondSlot = false;
        vdp.dmaVramSecondSlot = false;
        vdp.dmaCopySecondSlot = false;
        vdp.dmaAddress = vdp.address;
    }

    const VDPSlotTable& activeTimingTable = (vdp.activeWidth == 320) ? kH40Active : kH32Active;
    vdp.hblankStartCycle_ = activeTimingTable.hblankStartM68kCycle;

    // Bus state
    readArr(f, bus.ram, 0x10000);
    readArr(f, bus.z80Ram, 0x2000);
    readVal(f, bus.z80Bank);
    readArr(f, bus.romBankRegs, 8);
    readVal(f, bus.sramMapped);
    readArr(f, bus.ioData, 3);
    readArr(f, bus.ioCtrl, 3);
    readArr(f, bus.padState, 2);
    readArr(f, bus.padState6, 2);
    readArr(f, bus.thCounter, 2);
    if (version >= 6) {
        readArr(f, bus.thTimeoutLines, 2);
    } else {
        std::memset(bus.thTimeoutLines, 0, sizeof(bus.thTimeoutLines));
    }
    readArr(f, bus.prevTH, 2);
    readVal(f, bus.z80BusRequested);
    readVal(f, bus.z80Reset);

    // YM2612 state
    readArr(f, ym2612.channels, 6);
    readArr(f, ym2612.addressLatch, 2);
    readVal(f, ym2612.lfoFreq);
    readVal(f, ym2612.lfoEnabled);
    readVal(f, ym2612.dacData);
    readVal(f, ym2612.dacEnabled);
    ym2612.dacOut = (static_cast<s32>(ym2612.dacData) - 128) << 6;
    readVal(f, ym2612.timerA);
    readVal(f, ym2612.timerB);
    readVal(f, ym2612.timerControl);
    readVal(f, ym2612.timerAOverflow);
    readVal(f, ym2612.timerBOverflow);
    readVal(f, ym2612.timerACounter);
    readVal(f, ym2612.timerBCounter);
    if (version >= 12) {
        readVal(f, ym2612.timerBSubCounter);
    } else {
        ym2612.timerBSubCounter = 0;
    }
    readVal(f, ym2612.busyCounter);
    readVal(f, ym2612.egCounter);
    readVal(f, ym2612.egSubCounter);
    readVal(f, ym2612.ch3SpecialMode);
    readVal(f, ym2612.csmMode);
    readArr(f, ym2612.ch3Fnum, 3);
    readArr(f, ym2612.ch3Block, 3);
    readArr(f, ym2612.ch3Keycode, 3);
    readArr(f, ym2612.ch3FreqHiLatch, 3);
    readVal(f, ym2612.lfoCounter);
    readVal(f, ym2612.lfoPeriod);
    readVal(f, ym2612.lfoAM);
    readVal(f, ym2612.lfoPM);

    // PSG state
    readArr(f, psg.tone, 3);
    readVal(f, psg.noise);
    readVal(f, psg.lfsr);
    readVal(f, psg.latchedChannel);
    readVal(f, psg.latchedVolume);
    readVal(f, psg.clockCounter);
    if (version >= 12) {
        readVal(f, psg.sampleAccum);
        readVal(f, psg.sampleCount);
    } else {
        psg.sampleAccum = 0;
        psg.sampleCount = 0;
    }

    // Genesis internal state
    readVal(f, frameCount);
    readVal(f, audioSampleCounter);
    readVal(f, ymOutputTickCounter);
    readVal(f, z80CycleAccum);
    readVal(f, ymNativeTickAccum);
    readVal(f, m68kCycleDebt);
    readVal(f, z80CycleDebt);
    readVal(f, m68kLineAccum);
    if (version >= 12) {
        readArr(f, dcBlockPrevIn, 2);
        readArr(f, dcBlockPrevOut, 2);
        readArr(f, ymLpfPrevIn, 2);
        readArr(f, ymLpfPrevOut, 2);
    } else {
        std::memset(dcBlockPrevIn, 0, sizeof(dcBlockPrevIn));
        std::memset(dcBlockPrevOut, 0, sizeof(dcBlockPrevOut));
        std::memset(ymLpfPrevIn, 0, sizeof(ymLpfPrevIn));
        std::memset(ymLpfPrevOut, 0, sizeof(ymLpfPrevOut));
    }
    if (version >= 7) {
        readArr(f, ymNativeQueue, YM_NATIVE_QUEUE_SIZE);
        readVal(f, ymNativeQueueHead);
        readVal(f, ymNativeQueueTail);
        readVal(f, ymNativeQueueCount);
        readVal(f, ymLastSample);
    } else {
        clearYMSampleQueue();
    }
    if (version >= 2) {
        readVal(f, vdp.videoStandard_);
    } else {
        vdp.videoStandard_ = VideoStandard::NTSC;
    }
    const double loadedAudioSampleCounter = audioSampleCounter;
    const double loadedYMOutputTickCounter = ymOutputTickCounter;
    const int loadedZ80CycleAccum = z80CycleAccum;
    const int loadedYMNativeTickAccum = ymNativeTickAccum;
    const int loadedM68KCycleDebt = m68kCycleDebt;
    const int loadedZ80CycleDebt = z80CycleDebt;
    const int loadedM68KLineAccum = m68kLineAccum;
    YMSample loadedYMNativeQueue[YM_NATIVE_QUEUE_SIZE];
    std::memcpy(loadedYMNativeQueue, ymNativeQueue, sizeof(loadedYMNativeQueue));
    const int loadedYMNativeQueueHead = ymNativeQueueHead;
    const int loadedYMNativeQueueTail = ymNativeQueueTail;
    const int loadedYMNativeQueueCount = ymNativeQueueCount;
    const YMSample loadedYMLastSample = ymLastSample;

    bus.setVideoStandard(vdp.videoStandard_);
    setAudioSampleRate(audioRate);
    audioSampleCounter = loadedAudioSampleCounter;
    ymOutputTickCounter = loadedYMOutputTickCounter;
    z80CycleAccum = loadedZ80CycleAccum;
    ymNativeTickAccum = loadedYMNativeTickAccum;
    m68kCycleDebt = loadedM68KCycleDebt;
    z80CycleDebt = loadedZ80CycleDebt;
    m68kLineAccum = loadedM68KLineAccum;
    std::memcpy(ymNativeQueue, loadedYMNativeQueue, sizeof(loadedYMNativeQueue));
    ymNativeQueueHead = loadedYMNativeQueueHead;
    ymNativeQueueTail = loadedYMNativeQueueTail;
    ymNativeQueueCount = loadedYMNativeQueueCount;
    ymLastSample = loadedYMLastSample;

    if (version >= 13) {
        vdp.lineCycleBudget = loadedLineCycleBudget;
        vdp.hblankCounterDecremented_ = loadedHblankCounterDecremented;
        vdp.vblankIRQArmed = loadedVblankIRQArmed;
        vdp.vblankIRQAssertCycle = loadedVblankIRQAssertCycle;
    } else {
        // Reconstruct the current line's 488/489-cycle budget from the saved
        // remainder after line-start division.
        vdp.lineCycleBudget = (m68kLineAccum <= 3) ? 489 : 488;
        vdp.hblankCounterDecremented_ =
            (vdp.scanline < vdp.activeHeight) && (vdp.lineCycles >= vdp.hblankStartCycle_);
        vdp.refreshVBlankIRQState();
    }

    const bool canonicalizedBoundaryState = canonicalizeLoadedBoundaryState();

    // Skip thumbnail (we just need the state)
    // The framebuffer will be rendered fresh on the next frame

    paused = false;
    audioBufferPos = 0;
    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    std::memset(dcBlockPrevIn, 0, sizeof(dcBlockPrevIn));
    std::memset(dcBlockPrevOut, 0, sizeof(dcBlockPrevOut));
    std::memset(ymLpfPrevIn, 0, sizeof(ymLpfPrevIn));
    std::memset(ymLpfPrevOut, 0, sizeof(ymLpfPrevOut));
    m68k.invalidatePrefetch();
    resetBadSplitTraceState();
    static const bool logStateLoad = envEnabled("GENESIS_LOG_STATE_LOAD");
    if (logStateLoad) {
        std::fprintf(stderr,
                     "[STATE-LOAD] frame=%d scanline=%d lineCyc=%d pc=%06X sr=%04X debt=%d lineAccum=%d fe10=%02X fe11=%02X ffd8w=%04X ffd8=%02X ffd9=%02X dispEn=%d dispLat=%d hint=%d vint=%d slot=%d canon=%d budget=%d\n",
                     frameCount,
                     vdp.scanline,
                     vdp.lineCycles,
                     m68k.getPC(),
                     m68k.getSR(),
                     m68kCycleDebt,
                     m68kLineAccum,
                     bus.ram[0xFE10],
                     bus.ram[0xFE11],
                     (static_cast<unsigned>(bus.ram[0xFFD8]) << 8) | bus.ram[0xFFD9],
                     bus.ram[0xFFD8],
                     bus.ram[0xFFD9],
                     vdp.displayEnabled ? 1 : 0,
                     vdp.displayEnabledLatch ? 1 : 0,
                     vdp.hblankIRQ ? 1 : 0,
                     vdp.vblankIRQ ? 1 : 0,
                     vdp.currentSlotIndex,
                     canonicalizedBoundaryState ? 1 : 0,
                     vdp.lineCycleBudget);
    }

    return f.good();
}

bool Genesis::loadStateThumbnail(int slot, u32* thumbnailOut) const {
    std::string path = getStatePath(slot);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;

    auto fileSize = f.tellg();
    auto thumbBytes = static_cast<std::streamoff>(THUMB_W * THUMB_H * sizeof(u32));
    if (fileSize < thumbBytes) return false;

    // Thumbnail is at the end of the file
    f.seekg(-thumbBytes, std::ios::end);
    f.read(reinterpret_cast<char*>(thumbnailOut), THUMB_W * THUMB_H * sizeof(u32));
    return f.good();
}

void Genesis::setButton(int port, int button, bool pressed) {
    if (port < 0 || port > 1) return;

    // Button mapping:
    // 0=Up, 1=Down, 2=Left, 3=Right, 4=A, 5=B, 6=C, 7=Start
    // 8=X, 9=Y, 10=Z, 11=Mode (6-button extra)
    if (button >= 0 && button < 12) {
        if (button < 8) {
            if (pressed) {
                controllerData[port] &= ~(1 << button);
            } else {
                controllerData[port] |= (1 << button);
            }
        }
        bus.setButtonState(port, button, pressed);
    }
}
