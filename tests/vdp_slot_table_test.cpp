// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only
//
// Slot-table structural tests. These lock the invariants the renderer
// and the slot-driven dispatch loop depend on, including the SLOT_NT_A
// capture points consumed by per-column vscroll rendering.

#include "video/vdp_slot_table.h"

#include <cstdio>

static int totalTests = 0;
static int passedTests = 0;
static int failedTests = 0;

static void check(bool condition, const char* desc) {
    totalTests++;
    if (condition) {
        passedTests++;
        return;
    }
    failedTests++;
    std::printf("FAIL: %s\n", desc);
}

// Slot-driven dispatch (clockM68K slow path) reads info[i].type to pick
// which handler to run. SLOT_NT_A must be visible via info[] for per-column
// vscroll capture. These tests lock the number of per-column NT_A slots
// in H40 (17 column blocks) and H32 (12 column blocks).
static void testH40NtAIsDispatchable() {
    int ntACount = 0;
    for (int i = 0; i < kH40Active.count; i++) {
        if (kH40Active.info[i].type == SLOT_NT_A) {
            ntACount++;
            check(kH40Active.info[i].column < 42,
                  "H40 SLOT_NT_A has a valid column index");
            check(kH40Active.nextActionSlot[i] == i,
                  "H40 SLOT_NT_A is an action slot (clockM68K will dispatch)");
        }
    }
    check(ntACount == 17,
          "H40 has exactly 17 NT_A slots -- one per column block");
}

static void testH32NtAIsDispatchable() {
    int ntACount = 0;
    for (int i = 0; i < kH32Active.count; i++) {
        if (kH32Active.info[i].type == SLOT_NT_A) {
            ntACount++;
            check(kH32Active.info[i].column < 42,
                  "H32 SLOT_NT_A has a valid column index");
            check(kH32Active.nextActionSlot[i] == i,
                  "H32 SLOT_NT_A is an action slot (clockM68K will dispatch)");
        }
    }
    check(ntACount == 12,
          "H32 has exactly 12 NT_A slots -- one per column block");
}

// SLOT_NT_A column indices must be unique and cover the expected block
// range. A duplicate or missing index would make per-column captures
// skip a column (the renderer would then read a stale value at that
// colIdx).
static void testH40NtAColumnsUnique() {
    bool seen[42] = {};
    for (int i = 0; i < kH40Active.count; i++) {
        if (kH40Active.info[i].type == SLOT_NT_A) {
            uint8_t col = kH40Active.info[i].column;
            check(col < 42, "H40 NT_A column < 42");
            check(!seen[col], "H40 NT_A column indices are unique");
            seen[col] = true;
        }
    }
    // Column 0..16 expected
    for (int c = 0; c < 17; c++) {
        char desc[64];
        std::snprintf(desc, sizeof(desc), "H40 NT_A covers column %d", c);
        check(seen[c], desc);
    }
}

static void testH32NtAColumnsUnique() {
    bool seen[42] = {};
    for (int i = 0; i < kH32Active.count; i++) {
        if (kH32Active.info[i].type == SLOT_NT_A) {
            uint8_t col = kH32Active.info[i].column;
            check(col < 42, "H32 NT_A column < 42");
            check(!seen[col], "H32 NT_A column indices are unique");
            seen[col] = true;
        }
    }
    for (int c = 0; c < 12; c++) {
        char desc[64];
        std::snprintf(desc, sizeof(desc), "H32 NT_A covers column %d", c);
        check(seen[c], desc);
    }
}

// H32 full-screen vscroll is sampled at the scanline boundary before
// same-line H-int work can rewrite VSRAM for future lines. The old
// active-display latch slot must stay absent.
static void testH32HasNoActiveDisplayVscrollLatch() {
    int latchSlots = 0;
    for (int i = 0; i < kH32Active.count; i++) {
        if (kH32Active.types[i] == SLOT_VSCROLL_LATCH ||
            kH32Active.info[i].type == SLOT_VSCROLL_LATCH) {
            latchSlots++;
        }
    }
    check(latchSlots == 0, "H32 active table has no mid-line vscroll latch slots");
}

static void testH40NoLegacyLatchAtSlot42() {
    // The old slot-42 hack is gone. Hardware-correct rendering for
    // H40 full-screen mode reads per-column from columnVscroll[] at
    // render time, so no single-point latch is required.
    check(kH40Active.types[42] != SLOT_VSCROLL_LATCH,
          "H40 slot 42 is no longer the full-screen vscroll latch "
          "(per-column capture supersedes line-wide latch)");
    check(kH40Active.info[42].type != SLOT_VSCROLL_LATCH,
          "H40 info[42] is not SLOT_VSCROLL_LATCH");
}

int main() {
    testH40NtAIsDispatchable();
    testH32NtAIsDispatchable();
    testH40NtAColumnsUnique();
    testH32NtAColumnsUnique();
    testH32HasNoActiveDisplayVscrollLatch();
    testH40NoLegacyLatchAtSlot42();

    if (failedTests == 0) {
        std::printf("All %d vdp slot table tests passed\n", totalTests);
        return 0;
    }

    std::printf("%d/%d vdp slot table tests failed\n", failedTests, totalTests);
    return 1;
}
