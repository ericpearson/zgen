// tests/m68k_refresh_test.cpp
// Locks M68K::applyRefresh* accounting against the calibrated refresh model.
// Refresh penalties are only charged at VDP/IO access sites, not at every
// scanline boundary. This test would have caught the 2026-04-19 Cotton
// black-sky bug where we added ~2000 extra cycles/frame from unconditional
// scanline-boundary refresh.

#include "../src/genesis.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failedTests = 0;
static int totalTests = 0;
static const char* kRefreshRomPath = "build/Panorama Cotton (Japan).md";

static bool fileExists(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

static bool check(bool cond, const char* desc) {
    totalTests++;
    if (!cond) {
        std::printf("    FAIL [%s]\n", desc);
        failedTests++;
        return false;
    }
    return true;
}

// Helper: boot a ROM and run N hardware frames with no input.
// Returns the total M68K cycle count at the end of frame N.
static uint64_t bootAndRun(const char* romPath, int frames) {
    Genesis gen;
    if (!gen.loadROM(romPath)) {
        std::fprintf(stderr, "failed to load ROM %s\n", romPath);
        std::exit(2);
    }
    gen.reset();
    for (int i = 0; i < frames; i++) {
        gen.runFrame();
    }
    return gen.getM68K().getTotalCycles();
}

static void testPerFrameCyclesMatchNtscBaseline() {
    std::printf("  per-frame cycles match NTSC baseline (no-input cold boot)\n");
    // NTSC H40 hw frame = 262 scanlines × 3420 MCLK = 896040 MCLK
    //                   = 128005.7 M68K cycles (7 MCLK per M68K cycle).
    // Over 20 frames the average should land within ±100 cycles of 128006.
    // On the pre-fix code this test fails at ~130000/frame (+2000 drift).
    const int frames = 20;
    uint64_t total = bootAndRun(kRefreshRomPath, frames);
    double per_frame = static_cast<double>(total) / frames;
    char desc[160];
    std::snprintf(desc, sizeof(desc),
                  "total=%llu / %d = %.0f per frame; expected 128006 ± 100",
                  static_cast<unsigned long long>(total), frames, per_frame);
    check(per_frame > 127906 && per_frame < 128106, desc);
}

static void testPerFrameCyclesWithHintEnabled() {
    std::printf("  per-frame cycles stable once HINT starts firing\n");
    // Same ROM, further into boot where HINT is actively firing. On pre-fix
    // code frames 6+ jumped to ~130000/frame; the hardware wall-clock target
    // stays at ~128000. We assert the frames-30-to-40 average (not first-N
    // to skip boot noise).
    Genesis gen;
    gen.loadROM(kRefreshRomPath);
    gen.reset();
    uint64_t at30 = 0;
    for (int i = 0; i < 40; i++) {
        gen.runFrame();
        if (i == 29) at30 = gen.getM68K().getTotalCycles();
    }
    uint64_t at40 = gen.getM68K().getTotalCycles();
    double per_frame_30_to_40 = static_cast<double>(at40 - at30) / 10.0;
    char desc[160];
    std::snprintf(desc, sizeof(desc),
                  "frames 30-40 avg = %.0f cycles; expected 128006 ± 100",
                  per_frame_30_to_40);
    check(per_frame_30_to_40 > 127906 && per_frame_30_to_40 < 128106, desc);
}

static void testRefreshNotChargedAtReset() {
    std::printf("  refresh not charged at reset (totalCycles==40)\n");
    // A clean reset should have totalCycles==40 (reset exception = 40
    // cycles per src/cpu/m68k.cpp:156). No refresh has accumulated yet.
    Genesis gen;
    gen.loadROM(kRefreshRomPath);
    gen.reset();
    uint64_t at_reset = gen.getM68K().getTotalCycles();
    char desc[160];
    std::snprintf(desc, sizeof(desc),
                  "totalCycles after reset = %llu (expected 40)",
                  static_cast<unsigned long long>(at_reset));
    check(at_reset == 40, desc);
}

static void testMasterCyclesInSyncWithTotalCycles() {
    std::printf("  masterCycles_ matches totalCycles_ * 7\n");
    // After N frames, masterCycles should track totalCycles * 7 exactly
    // (both advance at 7:1 ratio through applyRefresh and normal execution).
    Genesis gen;
    gen.loadROM(kRefreshRomPath);
    gen.reset();
    for (int i = 0; i < 10; i++) gen.runFrame();
    uint64_t total = gen.getM68K().getTotalCycles();
    uint64_t master = gen.getM68K().getMasterCycles();
    char desc[160];
    std::snprintf(desc, sizeof(desc),
                  "master=%llu vs total*7=%llu; delta=%lld",
                  static_cast<unsigned long long>(master),
                  static_cast<unsigned long long>(total * 7),
                  static_cast<long long>(master) - static_cast<long long>(total * 7));
    check(master == total * 7, desc);
}

int main() {
    std::printf("M68K refresh accounting tests\n");
    if (!fileExists(kRefreshRomPath)) {
        std::printf("SKIP: missing ROM fixture: %s\n", kRefreshRomPath);
        return 77;
    }
    testPerFrameCyclesMatchNtscBaseline();
    testPerFrameCyclesWithHintEnabled();
    testRefreshNotChargedAtReset();
    testMasterCyclesInSyncWithTotalCycles();

    if (failedTests == 0) {
        std::printf("\nAll %d tests passed\n", totalTests);
        return 0;
    }
    std::printf("\n%d/%d tests failed\n", failedTests, totalTests);
    return 1;
}
