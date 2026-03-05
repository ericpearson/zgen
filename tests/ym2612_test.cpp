// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/ym2612.h"

#include <cstdio>

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
    std::printf("  Testing %s...", name);
    std::fflush(stdout);
}

static void endGroup() {
    if (groupTests == groupPassed) {
        std::printf(" OK (%d tests)\n", groupTests);
    } else {
        std::printf(" FAILED (%d/%d passed)\n", groupPassed, groupTests);
    }
}

static bool check(bool condition, const char* desc) {
    totalTests++;
    groupTests++;
    if (condition) {
        passedTests++;
        groupPassed++;
        return true;
    }

    failedTests++;
    std::printf("\n    FAIL: %s [%s]", desc, currentGroup);
    return false;
}

class YM2612Test {
public:
    static void updateEnvelope(YM2612& ym, FMOperator& op, int keycode) {
        ym.updateEnvelope(op, keycode);
    }

    static int& busyCounter(YM2612& ym) { return ym.busyCounter; }
    static u16& timerA(YM2612& ym) { return ym.timerA; }
    static u8& timerB(YM2612& ym) { return ym.timerB; }
    static u8& timerControl(YM2612& ym) { return ym.timerControl; }
    static bool& timerAOverflow(YM2612& ym) { return ym.timerAOverflow; }
    static bool& timerBOverflow(YM2612& ym) { return ym.timerBOverflow; }
    static int& timerACounter(YM2612& ym) { return ym.timerACounter; }
    static int& timerBCounter(YM2612& ym) { return ym.timerBCounter; }
    static int& timerBSubCounter(YM2612& ym) { return ym.timerBSubCounter; }
    static FMChannel& channel(YM2612& ym, int index) { return ym.channels[index]; }
    static bool& dacEnabled(YM2612& ym) { return ym.dacEnabled; }
    static u8& dacData(YM2612& ym) { return ym.dacData; }
};

static void testDecayToSustainTransition() {
    beginGroup("decay-to-sustain transition");

    YM2612 ym;
    FMOperator op{};
    op.egPhase = 2;
    op.egLevel = 32;
    op.dr = 1;
    op.sl = 1;
    op.ks = 0;

    YM2612Test::updateEnvelope(ym, op, 0);
    check(op.egPhase == 3, "decay reaches sustain even when EG increment is zero");
    check(op.egLevel == 32, "decay clamps to sustain target");

    endGroup();
}

static void testBusyCounterOnNativeTicks() {
    beginGroup("busy counter native tick lifetime");

    YM2612 ym;
    ym.writeAddress(0x22, 0);
    ym.writeData(0x00, 0);
    check((ym.readStatus() & 0x80) != 0, "busy flag is set after register write");

    for (int i = 0; i < 31; i++) {
        ym.tick();
    }
    check((ym.readStatus() & 0x80) != 0, "busy flag remains set for first 31 native ticks");

    ym.tick();
    check((ym.readStatus() & 0x80) == 0, "busy flag clears on 32nd native tick");
    check(YM2612Test::busyCounter(ym) == 0, "busy counter reaches zero exactly");

    endGroup();
}

static void testTimerStepping() {
    beginGroup("timer stepping");

    YM2612 ym;
    YM2612Test::timerA(ym) = 1023;
    YM2612Test::timerACounter(ym) = 1023;  // counter counts up from register value
    YM2612Test::timerControl(ym) = 0x05;
    ym.tick();
    check(YM2612Test::timerAOverflow(ym), "timer A overflows on a native tick");

    ym.reset();
    YM2612Test::timerB(ym) = 255;
    YM2612Test::timerBCounter(ym) = 255;  // counter counts up from register value
    YM2612Test::timerControl(ym) = 0x0A;
    for (int i = 0; i < 7; i++) {
        ym.tick();
    }
    check(!YM2612Test::timerBOverflow(ym), "timer B does not overflow before 8 native ticks");
    ym.tick();
    check(YM2612Test::timerBOverflow(ym), "timer B overflows on the 8th native tick");

    endGroup();
}

static void testDACOverride() {
    beginGroup("DAC override");

    YM2612 ym;
    YM2612Test::dacEnabled(ym) = true;
    YM2612Test::dacData(ym) = 0xFF;
    YM2612Test::channel(ym, 5).pan = 0xC0;

    const YMSample sample = ym.tick();
    check(sample.left == 8160, "DAC overrides FM output on channel 6 left");
    check(sample.right == 8160, "DAC overrides FM output on channel 6 right");

    endGroup();
}

int main() {
    std::printf("Running YM2612 tests...\n");

    testDecayToSustainTransition();
    testBusyCounterOnNativeTicks();
    testTimerStepping();
    testDACOverride();

    std::printf("\nResults: %d/%d passed", passedTests, totalTests);
    if (failedTests == 0) {
        std::printf(" OK\n");
        return 0;
    }

    std::printf(" FAILED (%d failed)\n", failedTests);
    return 1;
}
