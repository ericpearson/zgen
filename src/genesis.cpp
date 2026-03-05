// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
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

} // namespace

Genesis::Genesis() : frameCount(0), paused(false),
                     detailedProfilingEnabled_(false),
                     audioRate(DEFAULT_AUDIO_RATE),
                     audioSamplesPerScanline(0.0), ymTicksPerOutputSample(0.0),
                     audioBufferPos(0), audioSampleCounter(0.0), ymOutputTickCounter(0.0),
                     z80CycleAccum(0), ymNativeTickAccum(0),
                     m68kCycleDebt(0), z80CycleDebt(0), m68kLineAccum(0),
                     ymNativeQueueHead(0), ymNativeQueueTail(0), ymNativeQueueCount(0),
                     ymLastSample{} {
    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    std::memset(ymNativeQueue, 0, sizeof(ymNativeQueue));
    std::memset(controllerData, 0x7F, sizeof(controllerData));
    std::memset(controllerCtrl, 0, sizeof(controllerCtrl));
    
    // Connect components
    m68k.connectBus(&bus);
    z80.setBus(&bus);
    vdp.connectBus(&bus);
    bus.connectVDP(&vdp);
    bus.connectZ80(&z80);
    bus.connectYM2612(&ym2612);
    bus.connectPSG(&psg);
    bus.connectCartridge(&cartridge);
    m68k.setInterruptAckCallback([this](int level) {
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
    ymNativeTickAccum += m68kCycles;
    int ymTicks = ymNativeTickAccum / 144;
    ymNativeTickAccum %= 144;
    for (int i = 0; i < ymTicks; i++) {
        pushYMSample(ym2612.tick());
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
    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    clearYMSampleQueue();

    std::memset(controllerData, 0x7F, sizeof(controllerData));
    std::memset(controllerCtrl, 0, sizeof(controllerCtrl));
    
    printf("Genesis reset complete. Starting execution at PC=%08X\n", m68k.getPC());
}

void Genesis::runFrame() {
    frameProfile_ = {};
    if (paused) return;

    audioBufferPos = 0;

    for (int line = 0; line < getScanlinesPerFrame(); line++) {
        bus.tickControllerProtocol();
        runScanline();
    }

    frameCount++;

    static const bool logFrames = []() {
        const char* env = std::getenv("GENESIS_LOG_FRAMES");
        return env && std::atoi(env) != 0;
    }();
    if (logFrames && (frameCount <= 5 || frameCount % 300 == 0)) {
        printf("Frame %d: PC=%08X SR=%04X (mask=%d) D0=%08X A7=%08X\n", 
               frameCount, m68k.getPC(), m68k.getSR(), 
               (m68k.getSR() >> 8) & 7,
               m68k.state.d[0], m68k.state.a[7]);
    }
}

void Genesis::runScanline() {
    const bool profile = detailedProfilingEnabled_;
    static const bool logHintTiming = []() {
        const char* env = std::getenv("GENESIS_LOG_HINT_TIMING");
        return env && std::atoi(env) != 0;
    }();
    static const bool logDividerM68K = []() {
        const char* env = std::getenv("GENESIS_LOG_M68K_DIVIDER");
        return env && std::atoi(env) != 0;
    }();

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
    }

    // Deliver pending interrupts before the 68K runs.
    if (vdp.vblankPending()) {
        m68k.interrupt(6);
        if (vdp.getScanline() == vdp.activeHeight) {
            z80.interrupt();
        }
    } else if (vdp.hblankPending()) {
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
    int z80CyclesRemaining = Z80_CYCLES_PER_SCANLINE + z80CycleDebt;
    
    while (m68kCyclesRemaining > 0) {
        // 68K→VDP DMA (modes 0/1) halts the 68K bus
        if (vdp.is68kDMABusy()) {
            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.clockM68K(1);
                frameProfile_.vdpMs += elapsedMs(vdpStart);

                auto psgStart = ProfileClock::now();
                psg.clock(1);
                frameProfile_.psgMs += elapsedMs(psgStart);
            } else {
                vdp.clockM68K(1);
                psg.clock(1);
            }
            if (profile) {
                auto ymStart = ProfileClock::now();
                clockYM(1);
                frameProfile_.ymMs += elapsedMs(ymStart);
            } else {
                clockYM(1);
            }
            if (!bus.z80Reset && !bus.z80BusRequested && z80CyclesRemaining > 0) {
                z80CycleAccum += M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                if (profile) {
                    auto z80Start = ProfileClock::now();
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                    frameProfile_.z80Ms += elapsedMs(z80Start);
                } else {
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                }
            } else if (bus.z80Reset || bus.z80BusRequested) {
                z80CycleAccum += M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                z80CyclesRemaining -= z80Cycles;
            }
            m68kCyclesRemaining--;
            continue;
        }

        // Stall 68K while VDP FIFO is full (real hardware holds /DTACK)
        while (vdp.isVDPFIFOFull() && m68kCyclesRemaining > 0) {
            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.clockM68K(1);
                frameProfile_.vdpMs += elapsedMs(vdpStart);

                auto psgStart = ProfileClock::now();
                psg.clock(1);
                frameProfile_.psgMs += elapsedMs(psgStart);
            } else {
                vdp.clockM68K(1);
                psg.clock(1);
            }
            if (profile) {
                auto ymStart = ProfileClock::now();
                clockYM(1);
                frameProfile_.ymMs += elapsedMs(ymStart);
            } else {
                clockYM(1);
            }
            // Z80 runs independently of 68K FIFO stalls
            if (!bus.z80Reset && !bus.z80BusRequested && z80CyclesRemaining > 0) {
                z80CycleAccum += M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                if (profile) {
                    auto z80Start = ProfileClock::now();
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                    frameProfile_.z80Ms += elapsedMs(z80Start);
                } else {
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                }
            } else if (bus.z80Reset || bus.z80BusRequested) {
                z80CycleAccum += M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                z80CyclesRemaining -= z80Cycles;
            }
            m68kCyclesRemaining--;
        }
        if (m68kCyclesRemaining <= 0) break;

        int cycles = 0;
        if (profile) {
            auto m68kStart = ProfileClock::now();
            cycles = m68k.execute();
            frameProfile_.m68kMs += elapsedMs(m68kStart);
        } else {
            cycles = m68k.execute();
        }
        if (cycles <= 0) {
            // CPU stopped or error
            break;
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
        }

        m68kCyclesRemaining -= cycles;
        if (profile) {
            auto vdpStart = ProfileClock::now();
            vdp.clockM68K(cycles);
            frameProfile_.vdpMs += elapsedMs(vdpStart);
        } else {
            vdp.clockM68K(cycles);
        }

        // Deliver newly-raised interrupts (H-int fires mid-line at HBlank)
        if (vdp.vblankPending()) {
            m68k.interrupt(6);
        } else if (vdp.hblankPending()) {
            if (logHintTiming) {
                std::fprintf(stderr, "[INLOOP-HINT] ln=%d lineCyc=%d mask=%d pendInt=%d budgetLeft=%d\n",
                             vdp.getScanline(),
                             vdp.lineCycles,
                             (m68k.getSR() >> 8) & 7,
                             m68k.state.pendingInterrupt,
                             m68kCyclesRemaining);
            }
            m68k.interrupt(4);
        }

        // Run PSG in sync with M68K
        if (profile) {
            auto psgStart = ProfileClock::now();
            psg.clock(cycles);
            frameProfile_.psgMs += elapsedMs(psgStart);
        } else {
            psg.clock(cycles);
        }

        if (profile) {
            auto ymStart = ProfileClock::now();
            clockYM(cycles);
            frameProfile_.ymMs += elapsedMs(ymStart);
        } else {
            clockYM(cycles);
        }

        // Run Z80 proportionally (M68K:Z80 ratio = 7.67:3.58 MHz = 15:7)
        if (!bus.z80Reset && !bus.z80BusRequested && z80CyclesRemaining > 0) {
            z80CycleAccum += cycles * M68K_DIVIDER;  // multiply by 7
            int z80Cycles = z80CycleAccum / Z80_DIVIDER;  // divide by 15
            z80CycleAccum %= Z80_DIVIDER;  // keep remainder
            if (profile) {
                auto z80Start = ProfileClock::now();
                while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                    int ran = z80.execute();
                    if (ran <= 0) break;
                    z80Cycles -= ran;
                    z80CyclesRemaining -= ran;
                }
                frameProfile_.z80Ms += elapsedMs(z80Start);
            } else {
                while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                    int ran = z80.execute();
                    if (ran <= 0) break;
                    z80Cycles -= ran;
                    z80CyclesRemaining -= ran;
                }
            }
        } else if (bus.z80Reset || bus.z80BusRequested) {
            z80CycleAccum += cycles * M68K_DIVIDER;
            int z80Cycles = z80CycleAccum / Z80_DIVIDER;
            z80CycleAccum %= Z80_DIVIDER;
            z80CyclesRemaining -= z80Cycles;
        }
    }

    // If H-int is pending but still masked (e.g., V-int handler not finished),
    // keep advancing the CPU briefly so the pending edge can be serviced at the
    // correct temporal position instead of slipping multiple scanlines.
    if (vdp.hblankPending() && ((m68k.getSR() >> 8) & 7) >= 4) {
        constexpr int MAX_HINT_CATCHUP = VDP_MAX_M68K_CYCLES * 16;
        int extraCycles = 0;
        while (extraCycles < MAX_HINT_CATCHUP && vdp.hblankPending()) {
            int cycles = 0;
            if (profile) {
                auto m68kStart = ProfileClock::now();
                cycles = m68k.execute();
                frameProfile_.m68kMs += elapsedMs(m68kStart);
            } else {
                cycles = m68k.execute();
            }
            if (cycles <= 0) {
                break;
            }

            m68kCyclesRemaining -= cycles;
            extraCycles += cycles;

            if (profile) {
                auto vdpStart = ProfileClock::now();
                vdp.clockM68K(cycles);
                frameProfile_.vdpMs += elapsedMs(vdpStart);
            } else {
                vdp.clockM68K(cycles);
            }

            if (vdp.vblankPending()) {
                m68k.interrupt(6);
            } else if (vdp.hblankPending()) {
                m68k.interrupt(4);
            }

            if (profile) {
                auto psgStart = ProfileClock::now();
                psg.clock(cycles);
                frameProfile_.psgMs += elapsedMs(psgStart);
            } else {
                psg.clock(cycles);
            }

            if (profile) {
                auto ymStart = ProfileClock::now();
                clockYM(cycles);
                frameProfile_.ymMs += elapsedMs(ymStart);
            } else {
                clockYM(cycles);
            }

            if (!bus.z80Reset && !bus.z80BusRequested && z80CyclesRemaining > 0) {
                z80CycleAccum += cycles * M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                if (profile) {
                    auto z80Start = ProfileClock::now();
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                    frameProfile_.z80Ms += elapsedMs(z80Start);
                } else {
                    while (z80Cycles > 0 && z80CyclesRemaining > 0) {
                        int ran = z80.execute();
                        if (ran <= 0) break;
                        z80Cycles -= ran;
                        z80CyclesRemaining -= ran;
                    }
                }
            } else if (bus.z80Reset || bus.z80BusRequested) {
                z80CycleAccum += cycles * M68K_DIVIDER;
                int z80Cycles = z80CycleAccum / Z80_DIVIDER;
                z80CycleAccum %= Z80_DIVIDER;
                z80CyclesRemaining -= z80Cycles;
            }
        }
    }

    // Save overshoot so next scanline starts with correct budget
    m68kCycleDebt = m68kCyclesRemaining;  // negative = overshoot
    z80CycleDebt = z80CyclesRemaining;

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
        // Real hardware attenuates PSG through 51kΩ resistors (~-6 dB vs YM2612)
        s32 psgSample = 0;
        if (profile) {
            auto psgStart = ProfileClock::now();
            psgSample = psg.getSample();
            frameProfile_.psgMs += elapsedMs(psgStart);
        } else {
            psgSample = psg.getSample();
        }
        psgSample >>= 1;  // ~-6 dB to match hardware mixing levels
        left += psgSample;
        right += psgSample;

        if (profile) {
            auto mixStart = ProfileClock::now();
            // Clamp to s16 range
            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;

            audioBuffer[audioBufferPos++] = static_cast<s16>(left);
            audioBuffer[audioBufferPos++] = static_cast<s16>(right);
            frameProfile_.mixMs += elapsedMs(mixStart);
        } else {
            // Clamp to s16 range
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

// Save state format: magic + version + components + thumbnail
static constexpr u32 SAVE_MAGIC = 0x47454E53; // "GENS"
static constexpr u32 SAVE_VERSION = 9;
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
#ifdef _WIN32
    const char* home = std::getenv("APPDATA");
    if (!home) home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home) home = ".";
    std::filesystem::path dir = std::filesystem::path(home) / ".genesis" / "saves";
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
    writeVal(f, vdp.dmaBytesRemaining);
    writeVal(f, vdp.dmaWordsRemaining);
    { double placeholder = 0.0; writeVal(f, placeholder); } // was dmaCycleAccumulator
    writeVal(f, vdp.dmaFillPending);
    writeVal(f, vdp.dmaFillValue);
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

    // Genesis internal state
    writeVal(f, frameCount);
    writeVal(f, audioSampleCounter);
    writeVal(f, ymOutputTickCounter);
    writeVal(f, z80CycleAccum);
    writeVal(f, ymNativeTickAccum);
    writeVal(f, m68kCycleDebt);
    writeVal(f, z80CycleDebt);
    writeVal(f, m68kLineAccum);
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
    m68k.invalidatePrefetch();

    // Z80 state
    readVal(f, z80.state);

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
    readVal(f, vdp.dmaBytesRemaining);
    readVal(f, vdp.dmaWordsRemaining);
    { double discard = 0.0; readVal(f, discard); } // was dmaCycleAccumulator
    readVal(f, vdp.dmaFillPending);
    readVal(f, vdp.dmaFillValue);
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
    readVal(f, ym2612.timerA);
    readVal(f, ym2612.timerB);
    readVal(f, ym2612.timerControl);
    readVal(f, ym2612.timerAOverflow);
    readVal(f, ym2612.timerBOverflow);
    readVal(f, ym2612.timerACounter);
    readVal(f, ym2612.timerBCounter);
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

    // Genesis internal state
    readVal(f, frameCount);
    readVal(f, audioSampleCounter);
    readVal(f, ymOutputTickCounter);
    readVal(f, z80CycleAccum);
    readVal(f, ymNativeTickAccum);
    readVal(f, m68kCycleDebt);
    readVal(f, z80CycleDebt);
    readVal(f, m68kLineAccum);
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

    // Skip thumbnail (we just need the state)
    // The framebuffer will be rendered fresh on the next frame

    paused = false;
    audioBufferPos = 0;
    std::memset(audioBuffer, 0, sizeof(audioBuffer));
    m68k.invalidatePrefetch();

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
