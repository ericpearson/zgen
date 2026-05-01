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

    // getSample() returns average (no >>2 normalization, clamped to s16)
    s16 sample = psg.getSample();
    s16 expectedSample = 8191;
    check(sample == expectedSample, "getSample() returns averaged value = 8191");

    // Accumulators reset after getSample()
    check(PSGTest::sampleAccum(psg) == 0, "Accumulator reset after getSample()");
    check(PSGTest::sampleCount(psg) == 0, "Count reset after getSample()");

    // Fallback: getSample() with no accumulated ticks uses point sample
    s16 fallback = psg.getSample();
    check(fallback == expectedSample, "Fallback point sample matches (8191)");

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
    // This is between the extremes +/-8191.
    check(avgSample > -8191 && avgSample < 8191,
          "Averaged sample is between extremes (anti-aliased)");

    endGroup();
}

// =====================================================================
// Test 8: Tone output frequency
// =====================================================================
static void testToneFrequency() {
    beginGroup("Tone output frequency");

    // With period N, output should toggle every N ticks.
    // Full cycle = 2N ticks. After 2N ticks starting from output=+1,
    // output should be back to +1 (toggled twice).
    int periods[] = {10, 100, 0x3FF};
    for (int period : periods) {
        PSG psg;
        psg.reset();
        PSGTest::tone(psg, 0).tone = period;
        PSGTest::tone(psg, 0).volume = 0;
        PSGTest::tone(psg, 0).counter = period;
        PSGTest::tone(psg, 0).output = 1;

        int toggleCount = 0;
        int prevOutput = 1;
        for (int t = 0; t < period * 4; t++) {
            PSGTest::tickOnce(psg);
            if (PSGTest::tone(psg, 0).output != prevOutput) {
                toggleCount++;
                prevOutput = PSGTest::tone(psg, 0).output;
            }
        }
        // In 4*period ticks, should toggle exactly 4 times (2 full cycles)
        char desc[80];
        snprintf(desc, sizeof(desc), "Period %d: 4 toggles in 4N ticks (got %d)", period, toggleCount);
        check(toggleCount == 4, desc);
    }

    endGroup();
}

// =====================================================================
// Test 9: White noise LFSR full cycle
// =====================================================================
static void testWhiteNoiseLFSRFullCycle() {
    beginGroup("White noise LFSR full cycle");

    // Sega variant: 16-bit LFSR with taps at bits 0 and 3.
    // This is NOT a maximal-length LFSR — cycle length is 57337 (not 2^16-1).
    u16 reg = 0x8000;
    int cycleLen = 0;
    for (int i = 0; i < 65536; i++) {
        bool feedback = ((reg >> 0) ^ (reg >> 3)) & 1;
        reg = (reg >> 1) | (static_cast<u16>(feedback) << 15);
        if (reg == 0x8000) {
            cycleLen = i + 1;
            break;
        }
    }
    check(cycleLen == 57337, "White noise LFSR cycle length = 57337");

    // Verify PSG LFSR matches
    PSG psg;
    psg.reset();
    PSGTest::noise(psg).tone = 4; // White noise
    for (int i = 0; i < 57337; i++) {
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);
    }
    check(PSGTest::lfsr(psg) == 0x8000, "PSG LFSR returns to seed after 57337 shifts");

    endGroup();
}

// =====================================================================
// Test 10: Clock conversion precision
// =====================================================================
static void testClockConversion() {
    beginGroup("Clock conversion precision");

    // 1 M68K cycle = 7 master clocks. PSG ticks every 240 master clocks.
    // So 240/7 ≈ 34.28 M68K cycles per PSG tick.
    PSG psg;
    psg.reset();
    PSGTest::clockCounter(psg) = 0;

    // 34 M68K cycles = 238 master clocks — not enough for a tick
    psg.clock(34);
    check(PSGTest::sampleCount(psg) == 0, "34 M68K cycles (238 master) = 0 ticks");

    // 1 more M68K cycle = 245 total — exactly 1 tick with 5 remainder
    psg.clock(1);
    check(PSGTest::sampleCount(psg) == 1, "35 M68K cycles (245 master) = 1 tick");
    check(PSGTest::clockCounter(psg) == 5, "Remainder = 5 master clocks");

    // Reset and test larger batch
    psg.reset();
    PSGTest::clockCounter(psg) = 0;
    // 240 M68K cycles = 1680 master = 7 PSG ticks exactly
    psg.clock(240);
    check(PSGTest::sampleCount(psg) == 7, "240 M68K cycles = 7 ticks");
    check(PSGTest::clockCounter(psg) == 0, "No remainder");

    endGroup();
}

// =====================================================================
// Test 11: Mid-period register change
// =====================================================================
static void testMidPeriodChange() {
    beginGroup("Mid-period register change");

    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 0).tone = 100;
    PSGTest::tone(psg, 0).volume = 0;
    PSGTest::tone(psg, 0).counter = 50; // Midway through period
    PSGTest::tone(psg, 0).output = 1;

    // Change period to 200 while counter is at 50
    PSGTest::tone(psg, 0).tone = 200;

    // Run 50 ticks to exhaust current counter
    for (int i = 0; i < 50; i++) {
        PSGTest::tickOnce(psg);
    }
    // Counter should have reached 0 and reloaded with NEW period (200)
    check(PSGTest::tone(psg, 0).output == -1, "Output toggled after old counter expired");
    check(PSGTest::tone(psg, 0).counter == 200, "Counter reloaded with new period (200)");

    endGroup();
}

// =====================================================================
// Test 12: Noise period rates
// =====================================================================
static void testNoisePeriodRates() {
    beginGroup("Noise period rates");

    // Mode 0: period 0x10, Mode 1: period 0x20, Mode 2: period 0x40
    int expectedPeriods[] = {0x10, 0x20, 0x40};
    for (int mode = 0; mode < 3; mode++) {
        PSG psg;
        psg.reset();
        psg.write(0xE0 | mode); // Periodic noise, rate = mode

        // Set counter to expected period, run that many ticks
        PSGTest::noise(psg).counter = expectedPeriods[mode];
        PSGTest::noise(psg).output = 1;

        for (int t = 0; t < expectedPeriods[mode]; t++) {
            PSGTest::tickOnce(psg);
        }

        char desc[80];
        snprintf(desc, sizeof(desc), "Mode %d: output toggles after 0x%X ticks",
                 mode, expectedPeriods[mode]);
        check(PSGTest::noise(psg).output == -1, desc);

        snprintf(desc, sizeof(desc), "Mode %d: counter reloads to 0x%X",
                 mode, expectedPeriods[mode]);
        check(PSGTest::noise(psg).counter == expectedPeriods[mode], desc);
    }

    endGroup();
}

// =====================================================================
// Test 13: Output amplitude range
// =====================================================================
static void testOutputRange() {
    beginGroup("Output amplitude range");

    // Single channel at max volume — getSample should return
    // the raw channel amplitude (no multi-channel normalization)
    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 0).tone = 0x3FF;
    PSGTest::tone(psg, 0).volume = 0; // max
    PSGTest::tone(psg, 0).counter = 0x3FF;
    PSGTest::tone(psg, 0).output = 1;

    // Tick once to accumulate
    PSGTest::tickOnce(psg);
    s16 sample = psg.getSample();

    // With only 1 channel active, output should be 8191 (not 8191/4)
    char desc[128];
    snprintf(desc, sizeof(desc),
             "Single channel max vol output = %d (want >= 4000 for audibility)", sample);
    check(sample >= 4000, desc);

    // All 4 channels at max volume, all positive output
    PSG psg2;
    psg2.reset();
    for (int i = 0; i < 3; i++) {
        PSGTest::tone(psg2, i).tone = 0x3FF;
        PSGTest::tone(psg2, i).volume = 0;
        PSGTest::tone(psg2, i).counter = 0x3FF;
        PSGTest::tone(psg2, i).output = 1;
    }
    PSGTest::noise(psg2).volume = 0;
    PSGTest::lfsr(psg2) = 0x8001; // bit 0 = 1, so (lfsr&1) = true = positive

    PSGTest::tickOnce(psg2);
    s16 maxSample = psg2.getSample();

    snprintf(desc, sizeof(desc),
             "All channels max positive output = %d (should not clip s16)", maxSample);
    check(maxSample > 0 && maxSample <= 32767, desc);

    endGroup();
}

// =====================================================================
// Test 14: Noise LFSR reset on register write
// =====================================================================
static void testNoiseLFSRReset() {
    beginGroup("Noise LFSR reset on register write");

    PSG psg;
    psg.reset();

    // Shift LFSR several times to move away from seed
    psg.write(0xE4); // White noise, rate 0
    for (int i = 0; i < 10; i++) {
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);
    }
    check(PSGTest::lfsr(psg) != 0x8000, "LFSR moved from seed after shifts");

    // Writing noise register should reset LFSR
    psg.write(0xE4); // Same mode, but register write
    check(PSGTest::lfsr(psg) == 0x8000, "LFSR reset to 0x8000 on noise register write");

    // Writing noise volume should NOT reset LFSR
    for (int i = 0; i < 5; i++) {
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);
    }
    u16 lfsrBefore = PSGTest::lfsr(psg);
    psg.write(0xF0); // Noise volume = 0 (max)
    check(PSGTest::lfsr(psg) == lfsrBefore, "LFSR unchanged after noise volume write");

    endGroup();
}

// =====================================================================
// Test 15: Reset state verification
// =====================================================================
static void testResetState() {
    beginGroup("Reset state verification");

    PSG psg;
    psg.reset();

    // Tone channels: volume=15 (silent), output=-1, tone=0, counter=0
    for (int i = 0; i < 3; i++) {
        char desc[80];
        snprintf(desc, sizeof(desc), "Tone[%d] volume = 15 (silent)", i);
        check(PSGTest::tone(psg, i).volume == 15, desc);
        snprintf(desc, sizeof(desc), "Tone[%d] output = -1", i);
        check(PSGTest::tone(psg, i).output == -1, desc);
        snprintf(desc, sizeof(desc), "Tone[%d] tone = 0", i);
        check(PSGTest::tone(psg, i).tone == 0, desc);
        snprintf(desc, sizeof(desc), "Tone[%d] counter = 0", i);
        check(PSGTest::tone(psg, i).counter == 0, desc);
    }

    // Noise channel
    check(PSGTest::noise(psg).volume == 15, "Noise volume = 15 (silent)");
    check(PSGTest::noise(psg).output == -1, "Noise output = -1");
    check(PSGTest::noise(psg).tone == 0, "Noise tone = 0");
    check(PSGTest::noise(psg).counter == 0, "Noise counter = 0");

    // LFSR and latching state
    check(PSGTest::lfsr(psg) == 0x8000, "LFSR = 0x8000");
    check(PSGTest::clockCounter(psg) == 0, "clockCounter = 0");
    check(PSGTest::sampleAccum(psg) == 0, "sampleAccum = 0");
    check(PSGTest::sampleCount(psg) == 0, "sampleCount = 0");

    // Verify reset clears state that was previously set
    PSG psg2;
    psg2.reset();
    PSGTest::tone(psg2, 0).tone = 0x3FF;
    PSGTest::tone(psg2, 0).volume = 5;
    PSGTest::lfsr(psg2) = 0x1234;
    PSGTest::clockCounter(psg2) = 100;
    psg2.reset();
    check(PSGTest::tone(psg2, 0).tone == 0, "Tone reset after non-zero");
    check(PSGTest::tone(psg2, 0).volume == 15, "Volume reset after non-default");
    check(PSGTest::lfsr(psg2) == 0x8000, "LFSR reset after corruption");
    check(PSGTest::clockCounter(psg2) == 0, "clockCounter reset after non-zero");

    endGroup();
}

// =====================================================================
// Test 16: Noise data byte updates noise register
// =====================================================================
static void testNoiseDataByteWrite() {
    beginGroup("Noise data byte write");

    PSG psg;
    psg.reset();

    // Latch noise register to mode 5 (white noise, rate 1)
    psg.write(0xE5);
    check(PSGTest::noise(psg).tone == 5, "Noise tone = 5 after latch write");

    u16 lfsrAfterLatch = PSGTest::lfsr(psg);
    check(lfsrAfterLatch == 0x8000, "LFSR reset by noise latch write");

    // Shift LFSR a few times to move away from seed
    for (int i = 0; i < 5; i++) {
        PSGTest::noise(psg).counter = 1;
        PSGTest::noise(psg).output = -1;
        PSGTest::tickOnce(psg);
    }
    u16 lfsrBefore = PSGTest::lfsr(psg);
    check(lfsrBefore != 0x8000, "LFSR moved from seed");

    // Data bytes update the low 3 bits of the latched noise register on Sega
    // PSGs. This also counts as a noise-register write, so the LFSR resets.
    psg.write(0x02);
    check(PSGTest::noise(psg).tone == 2, "Noise tone updated to 2 by data byte");
    check(PSGTest::lfsr(psg) == 0x8000, "LFSR reset by noise data byte write");

    // Another data byte should update again.
    PSGTest::lfsr(psg) = 0x1234;
    psg.write(0x07);
    check(PSGTest::noise(psg).tone == 7, "Noise tone updated to 7 by second data byte");
    check(PSGTest::lfsr(psg) == 0x8000, "Second noise data byte also resets LFSR");

    endGroup();
}

// =====================================================================
// Test 17: White noise all 3 rates
// =====================================================================
static void testWhiteNoiseRates() {
    beginGroup("White noise rates");

    // White noise modes: tone values 4, 5, 6 map to periods 0x10, 0x20, 0x40
    int expectedPeriods[] = {0x10, 0x20, 0x40};
    for (int rate = 0; rate < 3; rate++) {
        PSG psg;
        psg.reset();
        psg.write(0xE0 | (4 + rate)); // White noise (bit 2 set), rate = 0,1,2

        PSGTest::noise(psg).counter = expectedPeriods[rate];
        PSGTest::noise(psg).output = 1;

        for (int t = 0; t < expectedPeriods[rate]; t++) {
            PSGTest::tickOnce(psg);
        }

        char desc[80];
        snprintf(desc, sizeof(desc), "White rate %d: output toggles after 0x%X ticks",
                 rate, expectedPeriods[rate]);
        check(PSGTest::noise(psg).output == -1, desc);

        snprintf(desc, sizeof(desc), "White rate %d: counter reloads to 0x%X",
                 rate, expectedPeriods[rate]);
        check(PSGTest::noise(psg).counter == expectedPeriods[rate], desc);

        // Verify white noise flag is set (tone & 4)
        snprintf(desc, sizeof(desc), "White rate %d: tone bit 2 set (value %d)",
                 rate, PSGTest::noise(psg).tone);
        check((PSGTest::noise(psg).tone & 4) != 0, desc);
    }

    endGroup();
}

// =====================================================================
// Test 18: Noise mode 3 with tone2=0
// =====================================================================
static void testNoiseMode3Tone2Zero() {
    beginGroup("Noise mode 3 tone2=0");

    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 2).tone = 0;  // tone2 = 0
    psg.write(0xE3); // Noise mode 3 (periodic, linked to tone 2)

    // Force noise counter reload
    PSGTest::noise(psg).counter = 1;
    PSGTest::noise(psg).output = 1;
    PSGTest::tickOnce(psg);

    // Period 0 should be treated as 1 (same as tone channels)
    check(PSGTest::noise(psg).counter == 1,
          "Noise with tone2=0 reloads counter to 1 (period 0 = period 1)");
    check(PSGTest::noise(psg).output == -1, "Output toggled");

    endGroup();
}

// =====================================================================
// Test 19: Tone latch-only partial update
// =====================================================================
static void testToneLatchPartialUpdate() {
    beginGroup("Tone latch partial update");

    PSG psg;
    psg.reset();

    // Full 2-byte write to set tone to known value: (0x1A << 4) | 0x5 = 0x1A5
    psg.write(0x85); // Latch ch0 tone, low nibble = 5
    psg.write(0x1A); // Data: high 6 bits = 0x1A = 26
    check(PSGTest::tone(psg, 0).tone == 0x1A5, "Initial tone = 0x1A5");

    // Now send only a latch byte with different low nibble
    psg.write(0x8C); // Latch ch0 tone, low nibble = 0xC
    // High 6 bits should be preserved (0x1A), low 4 updated to 0xC
    check(PSGTest::tone(psg, 0).tone == 0x1AC,
          "Latch-only: low nibble changed to 0xC, high bits preserved (0x1AC)");

    // Another latch-only write
    psg.write(0x83); // Latch ch0 tone, low nibble = 3
    check(PSGTest::tone(psg, 0).tone == 0x1A3,
          "Second latch-only: low nibble changed to 3 (0x1A3)");

    // Data byte should update only the high 6 bits
    psg.write(0x3F); // Data: high 6 bits = 63 = 0x3F
    check(PSGTest::tone(psg, 0).tone == 0x3F3,
          "Data byte: high bits updated to 0x3F, low nibble preserved (0x3F3)");

    endGroup();
}

// =====================================================================
// Test 20: All 4 channels volume independence
// =====================================================================
static void testVolumeIndependence() {
    beginGroup("Volume independence");

    PSG psg;
    psg.reset();

    // Set different volumes on each channel
    psg.write(0x92); // Ch0 vol = 2
    psg.write(0xB5); // Ch1 vol = 5
    psg.write(0xD9); // Ch2 vol = 9
    psg.write(0xFC); // Noise vol = 12

    check(PSGTest::tone(psg, 0).volume == 2, "Ch0 volume = 2");
    check(PSGTest::tone(psg, 1).volume == 5, "Ch1 volume = 5");
    check(PSGTest::tone(psg, 2).volume == 9, "Ch2 volume = 9");
    check(PSGTest::noise(psg).volume == 12, "Noise volume = 12");

    // Rewrite ch1 volume, verify others unchanged
    psg.write(0xB0); // Ch1 vol = 0 (max)
    check(PSGTest::tone(psg, 0).volume == 2, "Ch0 still 2 after ch1 write");
    check(PSGTest::tone(psg, 1).volume == 0, "Ch1 updated to 0");
    check(PSGTest::tone(psg, 2).volume == 9, "Ch2 still 9 after ch1 write");
    check(PSGTest::noise(psg).volume == 12, "Noise still 12 after ch1 write");

    // Rewrite noise volume, verify tone channels unchanged
    psg.write(0xF1); // Noise vol = 1
    check(PSGTest::tone(psg, 0).volume == 2, "Ch0 still 2 after noise write");
    check(PSGTest::tone(psg, 1).volume == 0, "Ch1 still 0 after noise write");
    check(PSGTest::tone(psg, 2).volume == 9, "Ch2 still 9 after noise write");
    check(PSGTest::noise(psg).volume == 1, "Noise updated to 1");

    endGroup();
}

// =====================================================================
// Test 21: Noise output polarity (LFSR bit 0) - UNIPOLAR model
// Hardware PSG outputs 0 when low, +vol when high (not symmetric -vol/+vol)
// =====================================================================
static void testNoiseOutputPolarity() {
    beginGroup("Noise output polarity");

    PSG psg;
    psg.reset();

    // Set only noise channel audible, all tone channels silent
    PSGTest::noise(psg).volume = 0; // max
    PSGTest::noise(psg).counter = 100; // Prevent counter reload during tick
    for (int i = 0; i < 3; i++) {
        PSGTest::tone(psg, i).volume = 15; // silent
        PSGTest::tone(psg, i).counter = 100;
    }

    // LFSR = 0x8001: bit 0 = 1 -> high -> positive output
    PSGTest::lfsr(psg) = 0x8001;
    PSGTest::tickOnce(psg);
    s16 sample1 = psg.getSample();
    check(sample1 > 0, "LFSR bit0=1: positive output");

    // LFSR = 0x8000: bit 0 = 0 -> low -> zero output (unipolar)
    PSGTest::lfsr(psg) = 0x8000;
    PSGTest::noise(psg).counter = 100;
    PSGTest::tickOnce(psg);
    s16 sample2 = psg.getSample();
    check(sample2 == 0, "LFSR bit0=0: zero output (unipolar)");

    // Unipolar: high = +vol, low = 0 (not symmetric)
    check(sample1 == 8191, "High state = max volume (8191)");

    endGroup();
}

// =====================================================================
// Test 22: getSample boundary (max sum, no overflow)
// =====================================================================
static void testGetSampleBoundary() {
    beginGroup("getSample boundary");

    // All 4 channels at max volume, all positive: 4 * 8191 = 32764 (fits in s16)
    PSG psg;
    psg.reset();
    for (int i = 0; i < 3; i++) {
        PSGTest::tone(psg, i).volume = 0;
        PSGTest::tone(psg, i).output = 1;
        PSGTest::tone(psg, i).tone = 0x3FF;
        PSGTest::tone(psg, i).counter = 0x3FF; // High counter prevents reload
    }
    PSGTest::noise(psg).volume = 0;
    PSGTest::noise(psg).counter = 0x3FF; // Prevent reload
    PSGTest::lfsr(psg) = 0x8001; // bit 0 = 1 -> positive

    PSGTest::tickOnce(psg);
    s16 maxPos = psg.getSample();
    check(maxPos == 32764, "All max positive = 4 * 8191 = 32764 (no clamp needed)");

    // All low: 0 (unipolar model - low state produces 0, not negative)
    for (int i = 0; i < 3; i++) {
        PSGTest::tone(psg, i).output = -1;
        PSGTest::tone(psg, i).counter = 0x3FF;
    }
    PSGTest::lfsr(psg) = 0x8000; // bit 0 = 0 -> low -> 0

    PSGTest::tickOnce(psg);
    s16 allLow = psg.getSample();
    check(allLow == 0, "All low = 0 (unipolar model)");

    // Verify clamp logic exists: manually set accum to overflow value
    PSGTest::sampleAccum(psg) = 40000;
    PSGTest::sampleCount(psg) = 1;
    s16 clamped = psg.getSample();
    check(clamped == 32767, "Accum 40000 clamped to 32767");

    PSGTest::sampleAccum(psg) = -40000;
    PSGTest::sampleCount(psg) = 1;
    s16 clampedNeg = psg.getSample();
    check(clampedNeg == -32768, "Accum -40000 clamped to -32768");

    endGroup();
}

// =====================================================================
// Test 23: Multiple getSample with no ticks (consistent point sample)
// =====================================================================
static void testGetSampleRepeatability() {
    beginGroup("getSample repeatability");

    PSG psg;
    psg.reset();
    PSGTest::tone(psg, 0).volume = 0;
    PSGTest::tone(psg, 0).output = 1;
    PSGTest::tone(psg, 0).tone = 0x3FF;
    PSGTest::tone(psg, 0).counter = 0x3FF;

    // No clocking — each getSample() should use fallback point sample
    s16 s1 = psg.getSample();
    s16 s2 = psg.getSample();
    s16 s3 = psg.getSample();

    check(s1 == s2, "Consecutive fallback samples match (1 vs 2)");
    check(s2 == s3, "Consecutive fallback samples match (2 vs 3)");
    check(s1 == 8191, "Fallback sample = 8191 (single ch max positive)");

    // Change output to low state, verify fallback updates (unipolar: low = 0)
    PSGTest::tone(psg, 0).output = -1;
    s16 s4 = psg.getSample();
    check(s4 == 0, "Fallback reflects output change to low (0, unipolar)");

    endGroup();
}

// =====================================================================
// Test 24: Large clock batch accuracy
// =====================================================================
static void testLargeClockBatch() {
    beginGroup("Large clock batch");

    PSG psg;
    psg.reset();
    PSGTest::clockCounter(psg) = 0;

    // 488 M68K cycles = one scanline
    // 488 * 7 = 3416 master clocks, 3416 / 240 = 14 ticks, remainder 56
    psg.clock(488);
    check(PSGTest::sampleCount(psg) == 14, "488 M68K cycles = 14 PSG ticks");
    check(PSGTest::clockCounter(psg) == 56, "Remainder = 56 master clocks");

    // Two scanlines back-to-back: remainder carries forward
    // 56 + 3416 = 3472 master, 3472 / 240 = 14 ticks, remainder 112
    psg.getSample(); // reset accumulators
    psg.clock(488);
    check(PSGTest::sampleCount(psg) == 14, "Second scanline: 14 more ticks");
    check(PSGTest::clockCounter(psg) == 112, "Remainder = 112 after two scanlines");

    // Third: 112 + 3416 = 3528, 3528/240 = 14 r168
    psg.getSample();
    psg.clock(488);
    check(PSGTest::sampleCount(psg) == 14, "Third scanline: 14 ticks");
    check(PSGTest::clockCounter(psg) == 168, "Remainder = 168 after three scanlines");

    endGroup();
}

// =====================================================================
// Test 25: Unipolar output model (hardware-correct)
// Hardware PSG outputs 0V when low, +V when high (unipolar).
// NOT symmetric around 0 (-V to +V) like our current bipolar model.
// =====================================================================
static void testUnipolarOutputModel() {
    beginGroup("Unipolar output model");

    PSG psg;
    psg.reset();

    // Set only tone channel 0 audible at max volume
    PSGTest::tone(psg, 0).volume = 0; // max
    PSGTest::tone(psg, 0).counter = 100; // Prevent reload during test
    for (int i = 1; i < 3; i++) {
        PSGTest::tone(psg, i).volume = 15; // silent
        PSGTest::tone(psg, i).counter = 100;
    }
    PSGTest::noise(psg).volume = 15; // silent
    PSGTest::noise(psg).counter = 100;

    // Test output when tone is HIGH (output = +1)
    PSGTest::tone(psg, 0).output = 1;
    PSGTest::tickOnce(psg);
    s16 sampleHigh = psg.getSample();
    check(sampleHigh > 0, "Tone HIGH: positive output");

    // Test output when tone is LOW (output = -1 in current code, but should produce 0)
    PSGTest::tone(psg, 0).output = -1;
    PSGTest::tone(psg, 0).counter = 100; // Reset counter
    PSGTest::tickOnce(psg);
    s16 sampleLow = psg.getSample();

    // HARDWARE BEHAVIOR: low state should produce 0, not negative
    // This is the key test - if sampleLow < 0, our model is wrong
    check(sampleLow == 0, "Tone LOW: zero output (unipolar, not negative)");

    // The ratio should be infinity (high/low = +vol/0), not -1 (bipolar)
    // For practical testing: low output must be exactly 0
    check(sampleLow >= 0, "Low state never negative (unipolar model)");

    endGroup();
}

// =====================================================================
// Test 26: Noise mode 3 dynamic tone2 sync via write()
// When noise is in mode 3, changing tone[2] frequency via register write
// should immediately update the noise period for subsequent reloads.
// =====================================================================
static void testNoiseMode3DynamicSyncViaWrite() {
    beginGroup("Noise mode 3 dynamic sync via write()");

    PSG psg;
    psg.reset();

    // Set tone 2 frequency to 0x42 via register writes
    psg.write(0xC2); // Latch ch2 tone, low nibble = 2
    psg.write(0x04); // Data byte, high 6 bits = 4 -> (4<<4)|2 = 0x42
    check(PSGTest::tone(psg, 2).tone == 0x42, "Tone 2 set to 0x42");

    // Enable noise mode 3 (periodic, linked to tone 2)
    psg.write(0xE3);

    // Force noise counter reload
    PSGTest::noise(psg).counter = 1;
    PSGTest::noise(psg).output = 1;
    PSGTest::tickOnce(psg);
    check(PSGTest::noise(psg).counter == 0x42, "Noise uses tone2 period 0x42");

    // Now change tone 2 frequency to 0x100 via register writes
    psg.write(0xC0); // Latch ch2 tone, low nibble = 0
    psg.write(0x10); // Data byte, high 6 bits = 16 -> (16<<4)|0 = 0x100
    check(PSGTest::tone(psg, 2).tone == 0x100, "Tone 2 changed to 0x100");

    // Force noise counter reload again
    PSGTest::noise(psg).counter = 1;
    PSGTest::noise(psg).output = 1;
    PSGTest::tickOnce(psg);

    // The noise channel MUST pick up the new tone2 period
    // This is the key test - current code does this correctly at clock time
    check(PSGTest::noise(psg).counter == 0x100,
          "Noise tracks tone2 period change to 0x100 (dynamic sync)");

    endGroup();
}

// =====================================================================
// Test 27: Noise unipolar output model
// Same as tone - hardware noise output is 0 or +vol, not -vol or +vol
// =====================================================================
static void testNoiseUnipolarOutput() {
    beginGroup("Noise unipolar output");

    PSG psg;
    psg.reset();

    // Set only noise channel audible at max volume
    PSGTest::noise(psg).volume = 0; // max
    PSGTest::noise(psg).counter = 100;
    for (int i = 0; i < 3; i++) {
        PSGTest::tone(psg, i).volume = 15; // silent
        PSGTest::tone(psg, i).counter = 100;
    }

    // LFSR bit 0 = 1 -> noise output should be +vol
    PSGTest::lfsr(psg) = 0x8001;
    PSGTest::tickOnce(psg);
    s16 sampleBit1 = psg.getSample();
    check(sampleBit1 > 0, "LFSR bit0=1: positive output");

    // LFSR bit 0 = 0 -> noise output should be 0 (unipolar), not -vol
    PSGTest::lfsr(psg) = 0x8000;
    PSGTest::noise(psg).counter = 100;
    PSGTest::tickOnce(psg);
    s16 sampleBit0 = psg.getSample();

    // HARDWARE BEHAVIOR: when LFSR bit 0 is 0, output should be 0
    check(sampleBit0 == 0, "LFSR bit0=0: zero output (unipolar, not negative)");
    check(sampleBit0 >= 0, "Noise low state never negative (unipolar model)");

    endGroup();
}

// =====================================================================
// Test: blip_buf band-limited synthesis integration
// =====================================================================
static void testBlipBufInit() {
    beginGroup("blip_buf initialization");

    PSG psg;
    psg.reset();

    // Initialize blip_buf with typical Genesis rates
    // PSG clock = master / 16 = 53693175 / 16 ≈ 3355823 Hz (NTSC)
    double psgClock = 53693175.0 / 16.0;
    double sampleRate = 44100.0;

    bool initOk = psg.initBlip(psgClock, sampleRate);
    check(initOk, "initBlip() returns true");

    // After init, no samples should be available yet
    check(psg.samplesAvailable() == 0, "No samples available before any clocking");

    psg.shutdownBlip();
    endGroup();
}

static void testBlipBufProducesSamples() {
    beginGroup("blip_buf produces samples");

    PSG psg;
    psg.reset();

    double psgClock = 53693175.0 / 16.0;  // ~3.36 MHz
    double sampleRate = 44100.0;
    psg.initBlip(psgClock, sampleRate);

    // Set up a tone on channel 0
    psg.write(0x8F);  // Latch ch0 tone, low nibble = F
    psg.write(0x00);  // Data byte, high bits = 0, so period = 0x00F = 15
    psg.write(0x90);  // Latch ch0 volume = 0 (max volume)

    // Clock for enough time to produce samples
    // At ~3.36 MHz PSG clock and 44.1 kHz output, we need ~76 PSG clocks per sample
    // Run for ~1000 samples worth = ~76000 PSG clocks
    // PSG clocks once per 240 master clocks, M68K runs at master/7
    // So 76000 PSG clocks = 76000 * 240 / 7 ≈ 2605714 M68K cycles
    // Let's just run a lot of M68K cycles
    for (int i = 0; i < 1000; i++) {
        psg.clock(2606);  // ~1000 samples worth
    }

    // End frame to make samples available
    // Clock duration in PSG clocks
    int psgClockDuration = 1000 * 76;  // Approximate
    psg.endFrame(psgClockDuration);

    int available = psg.samplesAvailable();
    check(available > 0, "Samples available after endFrame");

    // Read some samples
    short samples[100];
    int read = psg.readSamples(samples, 100);
    check(read > 0, "readSamples returns samples");

    psg.shutdownBlip();
    endGroup();
}

static void testBlipBufDCLevel() {
    beginGroup("blip_buf DC level correctness");

    PSG psg;
    psg.reset();

    double psgClock = 53693175.0 / 16.0;
    double sampleRate = 44100.0;
    psg.initBlip(psgClock, sampleRate);

    // Set all channels to silence (volume = 15 = off)
    psg.write(0x9F);  // Ch0 vol = 15
    psg.write(0xBF);  // Ch1 vol = 15
    psg.write(0xDF);  // Ch2 vol = 15
    psg.write(0xFF);  // Noise vol = 15

    // Clock and produce samples
    for (int i = 0; i < 100; i++) {
        psg.clock(2606);
    }
    psg.endFrame(100 * 76);

    short samples[100];
    int read = psg.readSamples(samples, 100);

    // With all channels silent, output should be near zero
    // (blip_buf may have some small transient at start)
    int nearZeroCount = 0;
    for (int i = 10; i < read; i++) {  // Skip first 10 for settling
        if (samples[i] >= -100 && samples[i] <= 100) {
            nearZeroCount++;
        }
    }
    check(nearZeroCount > (read - 20), "Silent channels produce near-zero output");

    psg.shutdownBlip();
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
    testToneFrequency();
    testWhiteNoiseLFSRFullCycle();
    testClockConversion();
    testMidPeriodChange();
    testNoisePeriodRates();
    testOutputRange();
    testNoiseLFSRReset();
    testResetState();
    testNoiseDataByteWrite();
    testWhiteNoiseRates();
    testNoiseMode3Tone2Zero();
    testToneLatchPartialUpdate();
    testVolumeIndependence();
    testNoiseOutputPolarity();
    testGetSampleBoundary();
    testGetSampleRepeatability();
    testLargeClockBatch();

    // Hardware-accurate behavior tests (TDD - these define correct behavior)
    testUnipolarOutputModel();
    testNoiseMode3DynamicSyncViaWrite();
    testNoiseUnipolarOutput();

    // blip_buf band-limited synthesis tests
    testBlipBufInit();
    testBlipBufProducesSamples();
    testBlipBufDCLevel();

    printf("\n=== Results: %d/%d passed", passedTests, totalTests);
    if (failedTests > 0) {
        printf(", %d FAILED", failedTests);
    }
    printf(" ===\n");

    return failedTests > 0 ? 1 : 0;
}
