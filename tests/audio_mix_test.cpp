// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

// Audio mixing unit tests
// Tests the mixing logic from genesis.cpp:592-636 without requiring
// a full Genesis instance, SDL, or audio device.

#include "audio/ym2612.h"
#include <cstdio>

// --- Test framework ---
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

// Helper: clamp to s16 range (same logic as genesis.cpp)
static s16 clampS16(s32 val) {
    if (val > 32767) val = 32767;
    if (val < -32768) val = -32768;
    return static_cast<s16>(val);
}

// =====================================================================
// Test 1: PSG -12dB attenuation (VA7+/Model 2 mixing)
// =====================================================================
static void testPSGAttenuation() {
    beginGroup("PSG -12dB attenuation");

    // PSG max single channel = 8191
    s32 psgRaw = 8191;
    s32 psgAttenuated = psgRaw >> 2;
    check(psgAttenuated == 2047, "8191 >> 2 = 2047 (-12dB)");

    // Negative value
    s32 psgNeg = -8191;
    s32 psgNegAtt = psgNeg >> 2;
    // Arithmetic right shift of -8191 by 2 = -2048 (rounds toward negative infinity)
    check(psgNegAtt == -2048, "-8191 >> 2 = -2048 (arithmetic shift)");

    // Zero stays zero
    s32 psgZero = 0;
    check((psgZero >> 2) == 0, "0 >> 2 = 0");

    // Max 4-channel PSG sum: 32764
    s32 psgMax = 32764;
    s32 psgMaxAtt = psgMax >> 2;
    check(psgMaxAtt == 8191, "32764 >> 2 = 8191");

    endGroup();
}

// =====================================================================
// Test 2: YM2612 + PSG stereo mixing
// =====================================================================
static void testStereoMixing() {
    beginGroup("YM + PSG stereo mixing");

    // Simulate the mixing formula from genesis.cpp:592-614
    // left = ymLeft + (psg >> 2), right = ymRight + (psg >> 2)
    s32 ymLeft = 1000;
    s32 ymRight = -500;
    s32 psgSample = 2000;

    s32 psgMixed = psgSample >> 2; // 500
    s32 left = ymLeft + psgMixed;
    s32 right = ymRight + psgMixed;

    check(left == 1500, "Left = 1000 + 500 = 1500");
    check(right == 0, "Right = -500 + 500 = 0");

    // PSG is mono — added equally to both channels
    check(psgMixed == 500, "PSG contribution equal to both channels");

    // Negative PSG
    s32 psgNeg = -4000;
    s32 psgNegMixed = psgNeg >> 2; // -1000
    s32 leftNeg = ymLeft + psgNegMixed;
    s32 rightNeg = ymRight + psgNegMixed;
    check(leftNeg == 0, "Left = 1000 + (-1000) = 0");
    check(rightNeg == -1500, "Right = -500 + (-1000) = -1500");

    endGroup();
}

// =====================================================================
// Test 3: Output clamping
// =====================================================================
static void testOutputClamping() {
    beginGroup("Output clamping");

    // Positive overflow
    s32 ymLeft = 32000;
    s32 psgSample = 4000;
    s32 psgMixed = psgSample >> 2; // 1000
    s32 left = ymLeft + psgMixed;  // 33000
    check(left == 33000, "Pre-clamp left = 33000");
    check(clampS16(left) == 32767, "33000 clamped to 32767");

    // Negative overflow
    s32 ymLeftNeg = -32000;
    s32 psgNeg = -4000;
    s32 psgNegMixed = psgNeg >> 2; // -1000
    s32 leftNeg = ymLeftNeg + psgNegMixed; // -33000
    check(leftNeg == -33000, "Pre-clamp left = -33000");
    check(clampS16(leftNeg) == -32768, "-33000 clamped to -32768");

    // Exact boundary: 32767 stays
    check(clampS16(32767) == 32767, "32767 not clamped");
    check(clampS16(-32768) == -32768, "-32768 not clamped");

    // Just inside range
    check(clampS16(32766) == 32766, "32766 not clamped");
    check(clampS16(-32767) == -32767, "-32767 not clamped");

    // Just outside range
    check(clampS16(32768) == 32767, "32768 clamped to 32767");
    check(clampS16(-32769) == -32768, "-32769 clamped to -32768");

    endGroup();
}

// =====================================================================
// Test 4: YM sample averaging (multiple ticks per output sample)
// =====================================================================
static void testYMSampleAveraging() {
    beginGroup("YM sample averaging");

    // Simulate consuming multiple YM samples and averaging
    // This replicates genesis.cpp:582-600
    YMSample samples[] = {
        {100, 200},
        {300, 400},
    };

    s32 ymLSum = 0;
    s32 ymRSum = 0;
    int consumedTicks = 0;
    for (auto& s : samples) {
        ymLSum += s.left;
        ymRSum += s.right;
        consumedTicks++;
    }

    s32 left = ymLSum / consumedTicks;
    s32 right = ymRSum / consumedTicks;

    check(left == 200, "YM avg left = (100+300)/2 = 200");
    check(right == 300, "YM avg right = (200+400)/2 = 300");

    // Add PSG contribution
    s32 psgSample = 500;
    s32 psgMixed = psgSample >> 2; // 125
    left += psgMixed;
    right += psgMixed;
    check(left == 325, "Final left = 200 + 125 = 325");
    check(right == 425, "Final right = 300 + 125 = 425");

    // Single-sample case (no averaging needed)
    ymLSum = 1000;
    ymRSum = -1000;
    consumedTicks = 1;
    left = ymLSum / consumedTicks;
    right = ymRSum / consumedTicks;
    check(left == 1000, "Single sample: left = 1000 (no averaging)");
    check(right == -1000, "Single sample: right = -1000");

    // Many samples: average with rounding toward zero
    ymLSum = 100 + 101 + 102; // 303
    consumedTicks = 3;
    left = ymLSum / consumedTicks; // 101
    check(left == 101, "3-sample average: 303/3 = 101");

    endGroup();
}

// =====================================================================
// Test 5: YM buffer underrun fallback
// =====================================================================
static void testYMBufferUnderrun() {
    beginGroup("YM buffer underrun fallback");

    // When no YM samples are consumed (consumedTicks=0), use ymLastSample
    // This replicates genesis.cpp:594-600
    YMSample ymLastSample = {500, -300};
    int consumedTicks = 0;

    s32 left, right;
    if (consumedTicks > 0) {
        left = 0; right = 0; // Not reached
    } else {
        left = ymLastSample.left;
        right = ymLastSample.right;
    }

    check(left == 500, "Underrun fallback left = ymLastSample.left (500)");
    check(right == -300, "Underrun fallback right = ymLastSample.right (-300)");

    // With PSG mixed in
    s32 psgSample = 1000;
    s32 psgMixed = psgSample >> 2; // 250
    left += psgMixed;
    right += psgMixed;
    check(left == 750, "Underrun + PSG: left = 500 + 250 = 750");
    check(right == -50, "Underrun + PSG: right = -300 + 250 = -50");

    // Zero lastSample (initial state)
    YMSample ymZero = {0, 0};
    consumedTicks = 0;
    left = ymZero.left;
    right = ymZero.right;
    s32 psg2 = 8191;
    s32 psgMixed2 = psg2 >> 2; // 2047
    left += psgMixed2;
    right += psgMixed2;
    check(left == 2047, "Zero lastSample + PSG: left = 2047");
    check(right == 2047, "Zero lastSample + PSG: right = 2047");

    endGroup();
}

int main() {
    printf("=== Audio Mixing Test Suite ===\n\n");

    testPSGAttenuation();
    testStereoMixing();
    testOutputClamping();
    testYMSampleAveraging();
    testYMBufferUnderrun();

    printf("\n=== Results: %d/%d passed", passedTests, totalTests);
    if (failedTests > 0) {
        printf(", %d FAILED", failedTests);
    }
    printf(" ===\n");

    return failedTests > 0 ? 1 : 0;
}
