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

    // Access VDP state via debug accessors
    const auto& vdp = genesis.getVDP();

    // Dump VDP state to binary file
    // Format: 65536 bytes VRAM, 128 bytes CRAM (64 x u16), 80 bytes VSRAM (40 x u16), 24 bytes regs
    std::FILE* bin = std::fopen(vdpBinPath, "wb");
    if (!bin) {
        std::fprintf(stderr, "Failed to open output: %s\n", vdpBinPath);
        return 1;
    }

    // VRAM: 65536 bytes (u8 array)
    std::fwrite(vdp.getVRAM(), 1, 0x10000, bin);

    // CRAM: pack scattered bus format (0BBB0GGG0RRR0) into GPGX's internal
    // 9-bit word layout (BBBGGGRRR) so vdp_compare consumes it correctly.
    for (int i = 0; i < 64; i++) {
        const std::uint16_t value = vdp.getCRAM()[i];
        const std::uint16_t packed =
            static_cast<std::uint16_t>(((value & 0x0E00) >> 3) |
                                       ((value & 0x00E0) >> 2) |
                                       ((value & 0x000E) >> 1));
        std::fwrite(&packed, sizeof(std::uint16_t), 1, bin);
    }

    // VSRAM: 40 x u16 = 80 bytes (little-endian)
    std::fwrite(vdp.getVSRAM(), sizeof(std::uint16_t), 40, bin);

    // Regs: 24 x u8
    std::fwrite(vdp.getRegs(), 1, 24, bin);

    // 68K RAM: 65536 bytes
    const auto& bus = genesis.getBus();
    std::fwrite(bus.getRam(), 1, 0x10000, bin);

    // Z80 RAM: 8192 bytes
    std::fwrite(bus.getZ80RamPtr(), 1, 0x2000, bin);

    // M68K state: d[8], a[8], pc, sr (for CPU execution in GPGX)
    const auto& m68kState = genesis.getM68K().getState();
    std::fwrite(m68kState.d, 4, 8, bin);  // 32 bytes
    std::fwrite(m68kState.a, 4, 8, bin);  // 32 bytes
    std::fwrite(&m68kState.pc, 4, 1, bin); // 4 bytes
    std::fwrite(&m68kState.sr, 2, 1, bin); // 2 bytes

    std::fclose(bin);
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
