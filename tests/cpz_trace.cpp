// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "genesis.h"
#include "debug_flags.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
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

void writeWavHeader(FILE* f, int sampleRate, int numSamples) {
    int dataSize = numSamples * 2 * sizeof(std::int16_t); // stereo s16
    int fileSize = 36 + dataSize;
    std::fwrite("RIFF", 1, 4, f);
    std::uint32_t v = fileSize; std::fwrite(&v, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    v = 16; std::fwrite(&v, 4, 1, f); // chunk size
    std::uint16_t u16 = 1; std::fwrite(&u16, 2, 1, f); // PCM
    u16 = 2; std::fwrite(&u16, 2, 1, f); // stereo
    v = sampleRate; std::fwrite(&v, 4, 1, f);
    v = sampleRate * 2 * 2; std::fwrite(&v, 4, 1, f); // byte rate
    u16 = 4; std::fwrite(&u16, 2, 1, f); // block align
    u16 = 16; std::fwrite(&u16, 2, 1, f); // bits per sample
    std::fwrite("data", 1, 4, f);
    v = dataSize; std::fwrite(&v, 4, 1, f);
}

}

// Input event for automated button presses.
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

    // Multi-frame dump: CPZ_DUMP_FRAMES="343:/tmp/f343.ppm,348:/tmp/f348.ppm"
    // Dumps PPMs at multiple specific frames in a single run.
    struct MultiDump { int frame; std::string path; };
    std::vector<MultiDump> multiDumps;
    if (const char* mdf = std::getenv("CPZ_DUMP_FRAMES")) {
        std::string s(mdf);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t colon = s.find(':', pos);
            if (colon == std::string::npos) break;
            size_t comma = s.find(',', colon);
            if (comma == std::string::npos) comma = s.size();
            int f = std::atoi(s.substr(pos, colon - pos).c_str());
            std::string p = s.substr(colon + 1, comma - colon - 1);
            multiDumps.push_back({f, p});
            pos = comma + 1;
        }
    }

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

    // Auto-detect PAL/NTSC from ROM header
    VideoStandard vs = genesis.getCartridge().preferredVideoStandard();
    genesis.setVideoStandard(vs);
    std::fprintf(stderr, "Video standard: %s (%d scanlines/frame)\n",
                 vs == VideoStandard::PAL ? "PAL" : "NTSC",
                 genesis.getScanlinesPerFrame());

    if (std::strcmp(statePath, "none") != 0) {
        if (!genesis.loadStateFromFile(statePath)) {
            std::fprintf(stderr, "failed to load state: %s\n", statePath);
            return 1;
        }
    }

    // CPZ_TRACE_DUMP_RAM=path: dump 68K RAM (64 KiB) to <path> right after
    // loading state, before running. Useful when comparing loaded-state RAM
    // against the first generated frame.
    if (const char* ramDumpPath = std::getenv("CPZ_TRACE_DUMP_RAM")) {
        std::FILE* rf = std::fopen(ramDumpPath, "wb");
        if (!rf) {
            std::fprintf(stderr, "failed to open RAM dump path: %s\n", ramDumpPath);
            return 1;
        }
        const u8* ram = genesis.getBus().getRam();
        std::fwrite(ram, 1, 0x10000, rf);
        std::fclose(rf);
        std::fprintf(stderr, "dumped 68K RAM (64 KiB) to %s\n", ramDumpPath);
    }

    // CPZ_TRACE_RAM_POKE="ADDR:VAL[,ADDR:VAL...]": after loading state,
    // poke individual RAM bytes. Used to test the Cotton frame-counter
    // hypothesis at 0xFF8000 (and similar) by editing state to match a
    // reference value. Addresses are 16-bit (RAM offset within 0..0xFFFF).
    if (const char* pokeSpec = std::getenv("CPZ_TRACE_RAM_POKE")) {
        const u8* ram_const = genesis.getBus().getRam();
        u8* ram = const_cast<u8*>(ram_const);
        const char* p = pokeSpec;
        while (*p) {
            char* endptr = nullptr;
            unsigned long addr = std::strtoul(p, &endptr, 16);
            if (endptr == p || *endptr != ':') break;
            p = endptr + 1;
            unsigned long val = std::strtoul(p, &endptr, 16);
            if (endptr == p) break;
            p = endptr;
            ram[addr & 0xFFFF] = static_cast<u8>(val & 0xFF);
            std::fprintf(stderr, "poke RAM[$%04lX] = $%02lX\n", addr & 0xFFFF, val & 0xFF);
            if (*p == ',') p++;
        }
    }

    // CPZ_TRACE_REG_POKE="REG:VAL[,REG:VAL...]": override M68K register
    // values after load. REG = A0..A7, D0..D7 (case-insensitive). VAL is hex.
    if (const char* regSpec = std::getenv("CPZ_TRACE_REG_POKE")) {
        const char* p = regSpec;
        while (*p) {
            if (*p == ',') { p++; continue; }
            if ((*p != 'A' && *p != 'a' && *p != 'D' && *p != 'd') || !std::isdigit(p[1])) {
                std::fprintf(stderr, "reg poke malformed at: %s\n", p);
                return 1;
            }
            char which = std::toupper(*p);
            int idx = p[1] - '0';
            if (idx < 0 || idx > 7) {
                std::fprintf(stderr, "reg index out of range\n");
                return 1;
            }
            p += 2;
            if (*p != ':') {
                std::fprintf(stderr, "reg poke missing ':' at: %s\n", p);
                return 1;
            }
            p++;
            char* endptr = nullptr;
            unsigned long val = std::strtoul(p, &endptr, 16);
            if (endptr == p) break;
            p = endptr;
            if (which == 'A') {
                genesis.getM68K().state.a[idx] = static_cast<u32>(val);
            } else {
                genesis.getM68K().state.d[idx] = static_cast<u32>(val);
            }
            std::fprintf(stderr, "poke M68K %c%d = $%08lX\n", which, idx, val);
        }
    }

    // CPZ_TRACE_RAM_OVERLAY="path:start:end[,path:start:end...]": after
    // loading state, overlay RAM bytes from a binary file over the
    // given range [start, end) (start and end are 16-bit RAM offsets).
    // The file is expected to be a 64 KiB RAM image. Used for region
    // bisection in the Cotton bug investigation: overlay halves of a
    // reference RAM dump into our save@22042, observe which half flips bug
    // to no-bug.
    if (const char* overlaySpec = std::getenv("CPZ_TRACE_RAM_OVERLAY")) {
        const u8* ram_const = genesis.getBus().getRam();
        u8* ram = const_cast<u8*>(ram_const);
        std::string spec(overlaySpec);
        size_t pos = 0;
        while (pos < spec.size()) {
            size_t comma = spec.find(',', pos);
            std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            size_t c1 = item.find(':');
            size_t c2 = item.find(':', c1 + 1);
            if (c1 == std::string::npos || c2 == std::string::npos) {
                std::fprintf(stderr, "overlay spec malformed: %s\n", item.c_str());
                return 1;
            }
            std::string path = item.substr(0, c1);
            unsigned long start = std::strtoul(item.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 16);
            unsigned long end = std::strtoul(item.substr(c2 + 1).c_str(), nullptr, 16);
            std::FILE* of = std::fopen(path.c_str(), "rb");
            if (!of) {
                std::fprintf(stderr, "overlay file not found: %s\n", path.c_str());
                return 1;
            }
            std::vector<u8> buf(0x10000);
            size_t n = std::fread(buf.data(), 1, 0x10000, of);
            std::fclose(of);
            if (n != 0x10000) {
                std::fprintf(stderr, "overlay file %s wrong size %zu (want 65536)\n", path.c_str(), n);
                return 1;
            }
            for (unsigned long a = start; a < end && a < 0x10000; a++) {
                ram[a] = buf[a];
            }
            std::fprintf(stderr, "overlay %s into RAM[$%04lX..$%04lX) (%lu bytes)\n",
                         path.c_str(), start, end, end - start);
            if (comma == std::string::npos) break;
            pos = comma + 1;
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
            if (!genesis.dumpVdpStateBin(vdpDumpBinPath, true)) {
                std::fprintf(stderr, "failed to dump VDP state to %s\n", vdpDumpBinPath);
                return 1;
            }
            std::fprintf(stderr, "dumped VDP state to %s\n", vdpDumpBinPath);
        }
        return 0;
    }

    // WAV dump: collect audio samples if GENESIS_DUMP_WAV is set
    const char* wavPath = std::getenv("GENESIS_DUMP_WAV");
    std::vector<std::int16_t> wavSamples;

    static constexpr int kFramebufferPitch = 320;
    static constexpr int kMaxPixels = kFramebufferPitch * 480;
    std::uint32_t prev[kMaxPixels] = {};
    std::uint32_t prev2[kMaxPixels] = {};
    bool havePrev = false;
    bool havePrev2 = false;

    for (int i = 0; i < frames; i++) {
        // Headless input semantics: release all buttons at the start of each
        // frame, then apply only the exact-frame event, if present.
        applyButtons(genesis, 0);
        while (nextInput < static_cast<int>(inputEvents.size()) &&
               inputEvents[nextInput].frame < i + 1) {
            nextInput++;
        }
        if (nextInput < static_cast<int>(inputEvents.size()) &&
            inputEvents[nextInput].frame == i + 1) {
            applyButtons(genesis, inputEvents[nextInput].buttons);
            nextInput++;
        }
        std::fprintf(stderr, "=== frame %d ===\n", i + 1);
        if (vdpDumpBinPath && dumpFrame == i + 1) {
            genesis.scheduleFrameBoundaryVdpDump(vdpDumpBinPath, true);
        }
        genesis.runFrame();
        // Collect audio samples for WAV dump
        if (wavPath) {
            int samples = genesis.getAudioSamples();
            const s16* buf = genesis.getAudioBuffer();
            wavSamples.insert(wavSamples.end(), buf, buf + samples * 2);
        }
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
        for (const auto& md : multiDumps) {
            if (md.frame == i + 1) {
                writePpm(md.path.c_str(), framebuffer, pitch, xOffset, width, height);
                std::fprintf(stderr, "dumped frame %d to %s\n", i + 1, md.path.c_str());
            }
        }
        if (vdpDumpBinPath && dumpFrame == i + 1) {
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

    // Write WAV file if requested
    if (wavPath && !wavSamples.empty()) {
        int totalSamples = static_cast<int>(wavSamples.size()) / 2; // stereo pairs
        FILE* wf = std::fopen(wavPath, "wb");
        if (wf) {
            writeWavHeader(wf, 48000, totalSamples);
            std::fwrite(wavSamples.data(), sizeof(std::int16_t), wavSamples.size(), wf);
            std::fclose(wf);
            std::fprintf(stderr, "wrote %d audio samples to %s\n", totalSamples, wavPath);
        }
    }

    return 0;
}
