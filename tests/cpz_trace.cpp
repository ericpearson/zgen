// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
#include "debug_flags.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {
std::uint64_t hashFramebuffer(const std::uint32_t* framebuffer,
                              int pitch,
                              int xOffset,
                              int width,
                              int height) {
    std::uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < height; y++) {
        const std::uint32_t* row = framebuffer + (y * pitch) + xOffset;
        for (int x = 0; x < width; x++) {
            hash ^= row[x];
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

int countLineDiffs(const std::uint32_t* a,
                   const std::uint32_t* b,
                   int pitch,
                   int xOffset,
                   int width,
                   int height) {
    int changed = 0;
    for (int y = 0; y < height; y++) {
        const std::uint32_t* rowA = a + (y * pitch) + xOffset;
        const std::uint32_t* rowB = b + (y * pitch) + xOffset;
        for (int x = 0; x < width; x++) {
            if (rowA[x] != rowB[x]) {
                changed++;
                break;
            }
        }
    }
    return changed;
}

void printDiffBounds(const std::uint32_t* a,
                     const std::uint32_t* b,
                     int pitch,
                     int xOffset,
                     int width,
                     int height) {
    int firstY = -1;
    int lastY = -1;
    int firstX = width;
    int lastX = -1;
    int changedPixels = 0;
    for (int y = 0; y < height; y++) {
        const std::uint32_t* rowA = a + (y * pitch) + xOffset;
        const std::uint32_t* rowB = b + (y * pitch) + xOffset;
        for (int x = 0; x < width; x++) {
            if (rowA[x] != rowB[x]) {
                if (firstY < 0) {
                    firstY = y;
                }
                lastY = y;
                if (x < firstX) firstX = x;
                if (x > lastX) lastX = x;
                changedPixels++;
            }
        }
    }
    if (firstY >= 0) {
        std::printf(" diffBox=%d..%d x %d..%d diffPixels=%d",
                    firstY,
                    lastY,
                    firstX,
                    lastX,
                    changedPixels);
    }
}

void writePpm(const char* path,
              const std::uint32_t* framebuffer,
              int pitch,
              int xOffset,
              int width,
              int height) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::perror("fopen");
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        const std::uint32_t* row = framebuffer + (y * pitch) + xOffset;
        for (int x = 0; x < width; x++) {
            unsigned char rgb[3] = {
                static_cast<unsigned char>((row[x] >> 16) & 0xFF),
                static_cast<unsigned char>((row[x] >> 8) & 0xFF),
                static_cast<unsigned char>(row[x] & 0xFF),
            };
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
}

void writeVdpStateBin(const char* path, Genesis& genesis) {
    std::ofstream bin(path, std::ios::binary);
    if (!bin.is_open()) {
        std::perror("ofstream");
        return;
    }

    const auto& vdp = genesis.getVDP();
    const auto& bus = genesis.getBus();
    const auto& m68kState = genesis.getM68K().getState();

    bin.write(reinterpret_cast<const char*>(vdp.getVRAM()), 0x10000);
    for (int i = 0; i < 64; i++) {
        const std::uint16_t value = vdp.getCRAM()[i];
        const std::uint16_t packed =
            static_cast<std::uint16_t>(((value & 0x0E00) >> 3) |
                                       ((value & 0x00E0) >> 2) |
                                       ((value & 0x000E) >> 1));
        bin.write(reinterpret_cast<const char*>(&packed), sizeof(packed));
    }
    bin.write(reinterpret_cast<const char*>(vdp.getVSRAM()), static_cast<std::streamsize>(sizeof(std::uint16_t) * 40));
    bin.write(reinterpret_cast<const char*>(vdp.getRegs()), 24);
    bin.write(reinterpret_cast<const char*>(bus.getRam()), 0x10000);
    bin.write(reinterpret_cast<const char*>(bus.getZ80RamPtr()), 0x2000);
    bin.write(reinterpret_cast<const char*>(m68kState.d), static_cast<std::streamsize>(sizeof(std::uint32_t) * 8));
    bin.write(reinterpret_cast<const char*>(m68kState.a), static_cast<std::streamsize>(sizeof(std::uint32_t) * 8));
    bin.write(reinterpret_cast<const char*>(&m68kState.pc), sizeof(m68kState.pc));
    bin.write(reinterpret_cast<const char*>(&m68kState.sr), sizeof(m68kState.sr));
}
}

// Input event for automated button presses (same format as GPGX headless_dump)
struct InputEvent { int frame; int buttons; }; // buttons: bitmask of button indices

static int parseButtons(const char* s) {
    int b = 0;
    for (; *s; s++) {
        switch (*s) {
            case 'U': b |= (1 << 0); break; // Up
            case 'D': b |= (1 << 1); break; // Down
            case 'L': b |= (1 << 2); break; // Left
            case 'R': b |= (1 << 3); break; // Right
            case 'A': b |= (1 << 4); break;
            case 'B': b |= (1 << 5); break;
            case 'C': b |= (1 << 6); break;
            case 'S': b |= (1 << 7); break; // Start
        }
    }
    return b;
}

static void applyButtons(Genesis& gen, int buttons) {
    for (int i = 0; i < 8; i++)
        gen.setButton(0, i, (buttons >> i) & 1);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: cpz_trace <rom> <state> [frames] [dump_frame dump_path] [frame:buttons ...]\n");
        return 2;
    }

    const char* romPath = argv[1];
    const char* statePath = argv[2];
    const int frames = (argc >= 4) ? std::atoi(argv[3]) : 8;
    const int dumpFrame = (argc >= 5) ? std::atoi(argv[4]) : -1;
    const char* dumpPath = (argc >= 6) ? argv[5] : nullptr;
    const char* saveStatePath = std::getenv("CPZ_TRACE_SAVE_STATE_PATH");
    const char* vdpDumpBinPath = std::getenv("CPZ_TRACE_VDP_DUMP_BIN");

    // Parse input events from --input-file or remaining args: frame:buttons (e.g. 1250:US 1260:)
    std::vector<InputEvent> inputEvents;
    bool inputFromFile = false;
    for (int a = 6; a < argc; a++) {
        if (std::strcmp(argv[a], "--input-file") == 0 && a + 1 < argc) {
            std::FILE* inf = std::fopen(argv[a + 1], "r");
            if (!inf) {
                std::fprintf(stderr, "failed to open input file: %s\n", argv[a + 1]);
                return 1;
            }
            char linebuf[256];
            while (std::fgets(linebuf, sizeof(linebuf), inf)) {
                const char* colon = std::strchr(linebuf, ':');
                if (!colon) continue;
                InputEvent ev;
                ev.frame = std::atoi(linebuf);
                ev.buttons = parseButtons(colon + 1);
                inputEvents.push_back(ev);
            }
            std::fclose(inf);
            inputFromFile = true;
            break;
        }
    }
    if (!inputFromFile) {
        for (int a = 6; a < argc; a++) {
            const char* colon = std::strchr(argv[a], ':');
            if (!colon) continue;
            InputEvent ev;
            ev.frame = std::atoi(argv[a]);
            ev.buttons = parseButtons(colon + 1);
            inputEvents.push_back(ev);
        }
    }
    int nextInput = 0;

    g_debugMode = true;

    Genesis genesis;
    genesis.setAudioSampleRate(48000);
    genesis.reset();

    if (!genesis.loadROM(romPath)) {
        std::fprintf(stderr, "failed to load ROM: %s\n", romPath);
        return 1;
    }
    genesis.setRomPath(romPath);

    if (std::strcmp(statePath, "none") != 0) {
        if (!genesis.loadStateFromFile(statePath)) {
            std::fprintf(stderr, "failed to load state: %s\n", statePath);
            return 1;
        }
    }

    // Immediate dump mode: frames=0 dumps the current state without running
    if (frames == 0 && dumpPath) {
        const int width = genesis.getViewportWidth();
        const int height = genesis.getScreenHeight();
        const int pitch = genesis.getFramebufferPitch();
        const int xOffset = genesis.getViewportXOffset();
        const std::uint32_t* framebuffer = genesis.getFramebuffer();
        writePpm(dumpPath, framebuffer, pitch, xOffset, width, height);
        std::fprintf(stderr, "immediate dump to %s (%dx%d)\n", dumpPath, width, height);
        if (vdpDumpBinPath) {
            writeVdpStateBin(vdpDumpBinPath, genesis);
            std::fprintf(stderr, "dumped VDP state to %s\n", vdpDumpBinPath);
        }
        return 0;
    }

    static constexpr int kFramebufferPitch = 320;
    static constexpr int kMaxPixels = kFramebufferPitch * 480;
    std::uint32_t prev[kMaxPixels] = {};
    std::uint32_t prev2[kMaxPixels] = {};
    bool havePrev = false;
    bool havePrev2 = false;

    for (int i = 0; i < frames; i++) {
        // Apply input events for this frame (1-indexed to match dump_frame)
        while (nextInput < static_cast<int>(inputEvents.size()) &&
               inputEvents[nextInput].frame <= i + 1) {
            applyButtons(genesis, inputEvents[nextInput].buttons);
            nextInput++;
        }
        std::fprintf(stderr, "=== frame %d ===\n", i + 1);
        genesis.runFrame();
        const int width = genesis.getViewportWidth();
        const int height = genesis.getScreenHeight();
        const int pitch = genesis.getFramebufferPitch();
        const int xOffset = genesis.getViewportXOffset();
        const std::uint32_t* framebuffer = genesis.getFramebuffer();
        const std::uint64_t frameHash = hashFramebuffer(framebuffer, pitch, xOffset, width, height);
        const std::uint16_t waterFlag = genesis.getBus().read16(0xFFF644);
        std::printf("frame=%d hash=%016llx height=%d",
                    i + 1,
                    static_cast<unsigned long long>(frameHash),
                    height);
        std::printf(" water=%04X", waterFlag);
        if (havePrev) {
            std::printf(" diffPrev=%d", countLineDiffs(framebuffer, prev, pitch, xOffset, width, height));
        }
        if (havePrev2) {
            std::printf(" diffPrev2=%d", countLineDiffs(framebuffer, prev2, pitch, xOffset, width, height));
            printDiffBounds(framebuffer, prev2, pitch, xOffset, width, height);
        }
        std::printf("\n");
        if (dumpPath && dumpFrame == i + 1) {
            writePpm(dumpPath, framebuffer, pitch, xOffset, width, height);
            std::fprintf(stderr, "dumped frame %d to %s\n", i + 1, dumpPath);
        }
        if (vdpDumpBinPath && dumpFrame == i + 1) {
            writeVdpStateBin(vdpDumpBinPath, genesis);
            std::fprintf(stderr, "dumped VDP state for frame %d to %s\n", i + 1, vdpDumpBinPath);
        }
        if (saveStatePath && dumpFrame == i + 1) {
            if (genesis.saveState(9)) {
                std::fprintf(stderr, "saved state for frame %d to slot 9 (%s)\n",
                             i + 1,
                             saveStatePath);
            } else {
                std::fprintf(stderr, "failed to save state for frame %d to slot 9 (%s)\n",
                             i + 1,
                             saveStatePath);
            }
        }

        const int rows = height;
        const int pitchPixels = pitch * rows;
        for (int p = 0; p < pitchPixels; p++) {
            prev2[p] = prev[p];
            prev[p] = framebuffer[p];
        }
        havePrev2 = havePrev;
        havePrev = true;
    }

    return 0;
}
