// VDP State Dumper
// Loads ROM + save state, runs 1 frame, dumps VDP state to binary + framebuffer as PPM.
//
// Usage: vdp_state_dump <rom> <state> <vdp_state.bin> <output.ppm>

#include "genesis.h"
#include "debug_flags.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void writePpm(const char* path,
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

int main(int argc, char* argv[]) {
    if (argc < 5) {
        std::fprintf(stderr, "Usage: %s <rom> <state> <vdp_state.bin> <output.ppm>\n", argv[0]);
        return 2;
    }

    const char* romPath = argv[1];
    const char* statePath = argv[2];
    const char* vdpBinPath = argv[3];
    const char* ppmPath = argv[4];

    g_debugMode = true;

    Genesis genesis;
    genesis.setAudioSampleRate(48000);
    genesis.reset();

    if (!genesis.loadROM(romPath)) {
        std::fprintf(stderr, "Failed to load ROM: %s\n", romPath);
        return 1;
    }
    genesis.setRomPath(romPath);

    if (!genesis.loadStateFromFile(statePath)) {
        std::fprintf(stderr, "Failed to load state: %s\n", statePath);
        return 1;
    }

    // Run 1 frame
    genesis.runFrame();

    if (!genesis.dumpVdpStateBin(vdpBinPath, true)) {
        std::fprintf(stderr, "Failed to write VDP state dump: %s\n", vdpBinPath);
        return 1;
    }
    std::fprintf(stderr, "Wrote VDP state + RAM + M68K to %s\n", vdpBinPath);

    // Dump framebuffer as PPM
    const int width = genesis.getViewportWidth();
    const int height = genesis.getScreenHeight();
    writePpm(ppmPath,
             genesis.getFramebuffer(),
             genesis.getFramebufferPitch(),
             genesis.getViewportXOffset(),
             width,
             height);
    std::fprintf(stderr, "Wrote framebuffer (%dx%d) to %s\n", width, height, ppmPath);

    return 0;
}
