// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// PSG (SN76489) unit test suite
// Self-contained: PSG only depends on types.h — no Bus stubs needed.

#include "audio/psg.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// --- Test framework (same pattern as m68k_test.cpp) ---
static int totalTests = 0;
static int passedTests = 0;
static int failedTests = 0;
static int groupTests = 0;
static int groupPassed = 0;
static const char* currentGroup = nullptr;

static void beginGroup(const char* name) {
    currentGroup = name;
    groupTests = 0;
    groupPassed = 0;
    printf("  Testing %s...", name);
    fflush(stdout);
}

static void endGroup() {
    if (groupTests == groupPassed) {
        printf(" OK (%d tests)\n", groupTests);
    } else {
        printf(" FAILED (%d/%d passed)\n", groupPassed, groupTests);
    }
}

static bool check(bool condition, const char* desc) {
    totalTests++;
    groupTests++;
    if (condition) {
        passedTests++;
        groupPassed++;
        return true;
    } else {
        failedTests++;
        printf("\n    FAIL: %s [%s]", desc, currentGroup);
        return false;
    }
}

// --- PSGTest: friend class for direct state inspection ---
class PSGTest {
public:
    static PSGChannel& tone(PSG& psg, int ch) { return psg.tone[ch]; }
    static PSGChannel& noise(PSG& psg) { return psg.noise; }
    static u16& lfsr(PSG& psg) { return psg.lfsr; }
    static int& clockCounter(PSG& psg) { return psg.clockCounter; }
    static s32& sampleAccum(PSG& psg) { return psg.sampleAccum; }
    static int& sampleCount(PSG& psg) { return psg.sampleCount; }
    static const s16* volumeTable() { return PSG::volumeTable; }

    // Advance PSG by exactly one tick
    static void tickOnce(PSG& psg) {
        psg.clockCounter = 233; // 233 + 7 = 240 -> exactly 1 PSG tick
        psg.clock(1);           // 1 M68K cycle * 7 = 7 master clocks
    }
};

// =====================================================================
// Test 1: Period 0 = Period 1
// =====================================================================
static void testPeriod0() {
    beginGroup("Period 0 = Period 1");

    // Period 0 should reload counter to 1 (not 0)
    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 0).tone = 0;
    PSGTest::tone(psg, 0).volume = 0;
    PSGTest::tone(psg, 0).counter = 1;
    PSGTest::tone(psg, 0).output = 1;

    PSGTest::tickOnce(psg);
    check(PSGTest::tone(psg, 0).counter == 1, "Period 0 reloads counter to 1");
    check(PSGTest::tone(psg, 0).output == -1, "Period 0 toggles output");

    // Verify identical behavior with period 1
    PSG psg1;
    psg1.reset();
    PSGTest::tone(psg1, 0).tone = 1;
    PSGTest::tone(psg1, 0).volume = 0;
    PSGTest::tone(psg1, 0).counter = 1;
    PSGTest::tone(psg1, 0).output = 1;

    PSGTest::tickOnce(psg1);
    check(PSGTest::tone(psg1, 0).counter == 1, "Period 1 reloads counter to 1");
    check(PSGTest::tone(psg1, 0).output == -1, "Period 1 toggles output");

    // Run both for many ticks — outputs must stay in lockstep
    for (int i = 0; i < 100; i++) {
        PSGTest::tickOnce(psg);
        PSGTest::tickOnce(psg1);
    }
    check(PSGTest::tone(psg, 0).output == PSGTest::tone(psg1, 0).output,
          "Period 0 and 1 match after 100 additional ticks");

    // Noise channel: period 0 via tone[2] link also reloads to 1
    PSG psgN;
    psgN.reset();
    PSGTest::tone(psgN, 2).tone = 0;
    psgN.write(0xE3); // Noise mode 3 (linked to tone 2)
    PSGTest::noise(psgN).counter = 1;
    PSGTest::noise(psgN).output = 1;

    PSGTest::tickOnce(psgN);
    check(PSGTest::noise(psgN).counter == 1, "Noise period 0 (via tone2) reloads to 1");

    endGroup();
}

// =====================================================================
// Test 2: Volume table (2 dB attenuation curve)
// =====================================================================
static void testVolumeTable() {
    beginGroup("Volume table");

    const s16* vt = PSGTest::volumeTable();

    check(vt[0] == 8191, "Entry 0 = 8191 (maximum)");
    check(vt[15] == 0, "Entry 15 = 0 (silence)");

    // Each step should attenuate by ~2 dB (ratio ~0.7943 = 10^(-2/20))
    for (int i = 0; i < 14; i++) {
        double ratio = static_cast<double>(vt[i + 1]) / static_cast<double>(vt[i]);
        double expected = pow(10.0, -2.0 / 20.0);
        double error = fabs(ratio - expected);
        char desc[128];
        snprintf(desc, sizeof(desc), "Step %d->%d: ratio %.4f (expected ~0.7943, err %.4f)",
                 i, i + 1, ratio, error);
        check(error < 0.02, desc);
    }

    // Monotonically decreasing
    for (int i = 0; i < 15; i++) {
        char desc[64];
        snprintf(desc, sizeof(desc), "Entry %d (%d) > entry %d (%d)", i, vt[i], i + 1, vt[i + 1]);
        check(vt[i] > vt[i + 1], desc);
    }

    endGroup();
}

// =====================================================================
// Test 3: White noise LFSR (Sega variant, taps 0 & 3)
// =====================================================================
static void testWhiteNoiseLFSR() {
    beginGroup("White noise LFSR");

    // Independently compute expected sequence
    u16 expected[32];
    u16 reg = 0x8000;
    for (int i = 0; i < 32; i++) {
        bool feedback = ((reg >> 0) ^ (reg >> 3)) & 1;
        reg = (reg >> 1) | (static_cast<u16>(feedback) << 15);
        expected[i] = reg;
    }

    // Drive PSG LFSR through 32 shifts
    PSG psg;
    psg.reset();
    PSGTest::noise(psg).tone = 4; // White noise, rate 0 (period 0x10)

    for (int i = 0; i < 32; i++) {
        // Force immediate rising-edge shift
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);

        char desc[80];
        snprintf(desc, sizeof(desc), "Shift %d: expected 0x%04X, got 0x%04X",
                 i, expected[i], PSGTest::lfsr(psg));
        check(PSGTest::lfsr(psg) == expected[i], desc);
    }

    endGroup();
}

// =====================================================================
// Test 4: Periodic noise LFSR (16-step single-bit rotation)
// =====================================================================
static void testPeriodicNoiseLFSR() {
    beginGroup("Periodic noise LFSR");

    PSG psg;
    psg.reset();
    PSGTest::noise(psg).tone = 0; // Periodic noise, rate 0

    u16 seed = PSGTest::lfsr(psg);
    check(seed == 0x8000, "Initial LFSR seed is 0x8000");

    // Single bit rotates through 16 positions then returns to seed
    u16 expectedValues[] = {
        0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080,
        0x0040, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001, 0x8000
    };

    for (int i = 0; i < 16; i++) {
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);

        char desc[80];
        snprintf(desc, sizeof(desc), "Step %d: expected 0x%04X, got 0x%04X",
                 i, expectedValues[i], PSGTest::lfsr(psg));
        check(PSGTest::lfsr(psg) == expectedValues[i], desc);
    }

    check(PSGTest::lfsr(psg) == seed, "LFSR returns to seed after 16 shifts");

    endGroup();
}

// =====================================================================
// Test 5: Noise mode 3 (tone channel 2 link)
// =====================================================================
static void testNoiseMode3() {
    beginGroup("Noise mode 3 (tone 2 link)");

    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 2).tone = 0x42;
    psg.write(0xE3); // Noise mode 3 (periodic, linked to tone 2)

    // Force noise counter reload
    PSGTest::noise(psg).counter = 1;
    PSGTest::noise(psg).output = 1;
    PSGTest::tickOnce(psg);

    check(PSGTest::noise(psg).counter == 0x42,
          "Noise reloads with tone[2] period (0x42)");

    // Change tone 2 and verify noise follows
    PSGTest::tone(psg, 2).tone = 0x100;
    PSGTest::noise(psg).counter = 1;
    PSGTest::noise(psg).output = 1;
    PSGTest::tickOnce(psg);

    check(PSGTest::noise(psg).counter == 0x100,
          "Noise tracks tone[2] period change (0x100)");

    endGroup();
}

// =====================================================================
// Test 6: Register protocol (latch/data writes)
// =====================================================================
static void testRegisterProtocol() {
    beginGroup("Register protocol");

    PSG psg;
    psg.reset();

    // 10-bit tone write: latch sets low 4 bits, data byte sets high 6 bits
    psg.write(0x85); // Latch ch0 tone, low nibble = 5
    psg.write(0x12); // Data byte, high 6 bits = 0x12 = 18
    // Expected: (18 << 4) | 5 = 0x125
    check(PSGTest::tone(psg, 0).tone == 0x125, "10-bit tone write: 0x125");

    // Volume write
    psg.write(0x9A); // Latch ch0 volume = 0xA = 10
    check(PSGTest::tone(psg, 0).volume == 10, "Volume write: ch0 vol = 10");
    check(PSGTest::tone(psg, 0).tone == 0x125, "Tone unchanged after volume write");

    // Channel switching: write to channel 2
    psg.write(0xC7); // Latch ch2 tone, low nibble = 7
    psg.write(0x3F); // Data byte, high 6 bits = 63
    // Expected: (63 << 4) | 7 = 0x3F7
    check(PSGTest::tone(psg, 2).tone == 0x3F7, "Channel 2 tone write: 0x3F7");
    check(PSGTest::tone(psg, 0).tone == 0x125, "Ch0 tone preserved after ch2 write");

    // Noise register write resets LFSR
    PSGTest::lfsr(psg) = 0x1234; // Corrupt LFSR
    psg.write(0xE5); // Noise register = 5 (white, rate 1)
    check(PSGTest::noise(psg).tone == 5, "Noise register write: 0x05");
    check(PSGTest::lfsr(psg) == 0x8000, "LFSR resets on noise register write");

    // Data byte updates latched register (volume)
    psg.write(0x93); // Latch ch0 volume = 3
    check(PSGTest::tone(psg, 0).volume == 3, "Latch sets ch0 volume to 3");
    psg.write(0x07); // Data byte updates latched (ch0 volume) = 7
    check(PSGTest::tone(psg, 0).volume == 7, "Data byte updates ch0 volume to 7");

    endGroup();
}

// =====================================================================
// Test 7: Sample averaging (anti-aliasing)
// =====================================================================
static void testSampleAveraging() {
    beginGroup("Sample averaging");

    PSG psg;
    psg.reset();

    // Ch0: max volume, large period (won't toggle in 7 ticks). Others off.
    PSGTest::tone(psg, 0).tone = 0x3FF;
    PSGTest::tone(psg, 0).volume = 0;
    PSGTest::tone(psg, 0).counter = 0x3FF;
    PSGTest::tone(psg, 0).output = 1;
    PSGTest::clockCounter(psg) = 0;

    // 240 M68K cycles * 7 = 1680 master clocks / 240 = exactly 7 PSG ticks
    psg.clock(240);

    check(PSGTest::sampleCount(psg) == 7, "7 PSG ticks accumulated");

    // Each tick: ch0 output=+1, vol=8191. Others vol=15(0). Mix = 8191.
    s32 expectedAccum = 8191 * 7;
    check(PSGTest::sampleAccum(psg) == expectedAccum, "Accumulator = 8191 * 7");

    // getSample() returns average >> 2
    s16 sample = psg.getSample();
    s16 expectedSample = static_cast<s16>(8191 >> 2); // 2047
    check(sample == expectedSample, "getSample() returns averaged value >> 2 = 2047");

    // Accumulators reset after getSample()
    check(PSGTest::sampleAccum(psg) == 0, "Accumulator reset after getSample()");
    check(PSGTest::sampleCount(psg) == 0, "Count reset after getSample()");

    // Fallback: getSample() with no accumulated ticks uses point sample
    s16 fallback = psg.getSample();
    check(fallback == expectedSample, "Fallback point sample matches (2047)");

    // Anti-aliasing: averaging with toggling output produces intermediate value
    PSG psg2;
    psg2.reset();
    PSGTest::tone(psg2, 0).tone = 1;
    PSGTest::tone(psg2, 0).volume = 0;
    PSGTest::tone(psg2, 0).counter = 1;
    PSGTest::tone(psg2, 0).output = 1;
    PSGTest::clockCounter(psg2) = 0;

    psg2.clock(240); // 7 ticks, output alternates: -,+,-,+,-,+,-
    s16 avgSample = psg2.getSample();

    // Average of alternating +/-8191 (4 negative, 3 positive) = -8191/7 = -1170
    // Then >>2 = -293. This is between the extremes +/-2047.
    check(avgSample > -(8191 >> 2) && avgSample < (8191 >> 2),
          "Averaged sample is between extremes (anti-aliased)");

    endGroup();
}

// =====================================================================
int main() {
    printf("=== PSG (SN76489) Test Suite ===\n\n");

    testPeriod0();
    testVolumeTable();
    testWhiteNoiseLFSR();
    testPeriodicNoiseLFSR();
    testNoiseMode3();
    testRegisterProtocol();
    testSampleAveraging();

    printf("\n=== Results: %d/%d passed", passedTests, totalTests);
    if (failedTests > 0) {
        printf(", %d FAILED", failedTests);
    }
    printf(" ===\n");

    return failedTests > 0 ? 1 : 0;
}
