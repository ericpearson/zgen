// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
std::uint64_t hashFramebuffer(const std::uint32_t* framebuffer, int pixels) {
    std::uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < pixels; i++) {
        hash ^= framebuffer[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

int countLineDiffs(const std::uint32_t* a, const std::uint32_t* b, int width, int height) {
    int changed = 0;
    for (int y = 0; y < height; y++) {
        const std::uint32_t* rowA = a + (y * width);
        const std::uint32_t* rowB = b + (y * width);
        for (int x = 0; x < width; x++) {
            if (rowA[x] != rowB[x]) {
                changed++;
                break;
            }
        }
    }
    return changed;
}

void printDiffBounds(const std::uint32_t* a, const std::uint32_t* b, int width, int height) {
    int firstY = -1;
    int lastY = -1;
    int firstX = width;
    int lastX = -1;
    int changedPixels = 0;
    for (int y = 0; y < height; y++) {
        const std::uint32_t* rowA = a + (y * width);
        const std::uint32_t* rowB = b + (y * width);
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
}

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: cpz_trace <rom> <state> [frames]\n");
        return 2;
    }

    const char* romPath = argv[1];
    const char* statePath = argv[2];
    const int frames = (argc >= 4) ? std::atoi(argv[3]) : 8;

    Genesis genesis;
    genesis.setAudioSampleRate(48000);
    genesis.reset();

    if (!genesis.loadROM(romPath)) {
        std::fprintf(stderr, "failed to load ROM: %s\n", romPath);
        return 1;
    }
    genesis.setRomPath(romPath);

    if (!genesis.loadStateFromFile(statePath)) {
        std::fprintf(stderr, "failed to load state: %s\n", statePath);
        return 1;
    }

    static constexpr int kMaxPixels = 320 * 480;
    std::uint32_t prev[kMaxPixels] = {};
    std::uint32_t prev2[kMaxPixels] = {};
    bool havePrev = false;
    bool havePrev2 = false;

    for (int i = 0; i < frames; i++) {
        std::fprintf(stderr, "=== frame %d ===\n", i + 1);
        genesis.runFrame();
        const int width = genesis.getScreenWidth();
        const int height = genesis.getScreenHeight();
        const int pixels = width * height;
        const std::uint32_t* framebuffer = genesis.getFramebuffer();
        const std::uint64_t frameHash = hashFramebuffer(framebuffer, pixels);
        const std::uint16_t waterFlag = genesis.getBus().read16(0xFFF644);
        std::printf("frame=%d hash=%016llx height=%d",
                    i + 1,
                    static_cast<unsigned long long>(frameHash),
                    height);
        std::printf(" water=%04X", waterFlag);
        if (havePrev) {
            std::printf(" diffPrev=%d", countLineDiffs(framebuffer, prev, width, height));
        }
        if (havePrev2) {
            std::printf(" diffPrev2=%d", countLineDiffs(framebuffer, prev2, width, height));
            printDiffBounds(framebuffer, prev2, width, height);
        }
        std::printf("\n");

        for (int p = 0; p < pixels; p++) {
            prev2[p] = prev[p];
            prev[p] = framebuffer[p];
        }
        havePrev2 = havePrev;
        havePrev = true;
    }

    return 0;
}
