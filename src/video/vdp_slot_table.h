// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

// Slot types for VDP internal memory access scheduling.
// Each scanline is divided into fixed-length "slots" where the VDP performs
// one memory access per slot. External slots are available for FIFO drains
// and DMA transfers; pattern/refresh slots are consumed internally.
enum VDPSlotType : uint8_t {
    SLOT_EXTERNAL,
    SLOT_REFRESH,
    SLOT_PATTERN,
    SLOT_VSCROLL_LATCH,  // Full-screen VSRAM latch point

    // Column block operations (sub-types of SLOT_PATTERN):
    SLOT_NT_A,           // Nametable fetch plane A
    SLOT_PAT_A0,         // Pattern data bytes 0-1 plane A
    SLOT_PAT_A1,         // Pattern data bytes 2-3 plane A
    SLOT_NT_B,           // Nametable fetch plane B
    SLOT_SPR_MAP,        // Sprite mapping (X/link) read
    SLOT_PAT_B0,         // Pattern data bytes 0-1 plane B
    SLOT_PAT_B1,         // Pattern data bytes 2-3 plane B + 8px output

    // Per-line operations:
    SLOT_HSCROLL,        // HSCROLL table read

    // Sprite pipeline:
    SLOT_SAT_SCAN,       // Sprite attribute table Y-range scan
    SLOT_SPR_PATTERN,    // Sprite pattern data read
};

// Per-slot metadata providing operation type and column block index.
// info[].type matches types[] for EXTERNAL, REFRESH, and VSCROLL_LATCH slots.
// For PATTERN slots, info[].type gives the specific sub-type.
struct VDPSlotInfo {
    VDPSlotType type = SLOT_PATTERN;
    uint8_t column = 0xFF;  // Column pair index (0-19 for H40), 0xFF for non-column
};

// Maximum slot count across all modes (H40 blank has the most).
static constexpr int VDP_MAX_SLOTS = 210;
static constexpr int VDP_MAX_M68K_CYCLES = 488;

struct VDPSlotTable {
    int count;                              // Total slots this scanline
    VDPSlotType types[VDP_MAX_SLOTS];       // Slot type at each position
    int mclkOffset[VDP_MAX_SLOTS];          // Master-clock offset (0..3419)
    int hintAssertSlot;                     // Slot where H-int counter/IRQ advances
    int hblankStartSlot;                    // First slot in HBlank
    int hblankEndSlot;                      // First slot after HBlank (start of active)
    int nextExternalSlot[VDP_MAX_SLOTS + 1]; // For slot i, index of next external slot (inclusive); count if none
    int nextActionSlot[VDP_MAX_SLOTS + 1];   // Next slot needing processing (EXTERNAL, VSCROLL_LATCH, or PAT_B1)
    int externalSlotCount;                  // Total external slots
    int pixelAtSlot[VDP_MAX_SLOTS];         // Visible pixel output position (activeWidth if blanking)
    int hcounterAtSlot[VDP_MAX_SLOTS];      // H-counter value at each slot
    int slotAtM68kCycle[VDP_MAX_M68K_CYCLES]; // Reverse lookup: M68K cycle -> slot index
    int hintAssertM68kCycle;                // First M68K cycle where slot >= hintAssertSlot
    int hblankStartM68kCycle;               // First M68K cycle where slot >= hblankStartSlot
    int activeWidth;                        // 320 or 256
    VDPSlotInfo info[VDP_MAX_SLOTS];        // Per-slot operation type and column metadata
};

// ---------------------------------------------------------------------------
// Table construction helpers
// ---------------------------------------------------------------------------

namespace vdp_slot_detail {

// Distribute external slots across a scanline. During active display, external
// slots are sparse (interleaved with pattern fetches). During HBlank they are
// dense. The positions are derived from Nemesis/SpritesMind hardware analysis
// and validated against the DMA bandwidth figures in the Sega Software Manual.

inline constexpr void fillPatternSlots(VDPSlotType* types, int start, int count) {
    for (int i = 0; i < count; i++) {
        types[start + i] = SLOT_PATTERN;
    }
}

template <int N>
inline constexpr void applyAnchoredTimings(VDPSlotTable& t,
                                           const int (&anchorSlotsIn)[N],
                                           const int (&anchorTimesIn)[N]) {
    int anchorSlots[N + 2] = {};
    int anchorTimes[N + 2] = {};
    anchorSlots[0] = 0;
    anchorTimes[0] = 0;
    for (int i = 0; i < N; i++) {
        anchorSlots[i + 1] = anchorSlotsIn[i];
        anchorTimes[i + 1] = anchorTimesIn[i];
    }
    anchorSlots[N + 1] = t.count - 1;
    anchorTimes[N + 1] = 3419;

    for (int span = 0; span < N + 1; span++) {
        const int startSlot = anchorSlots[span];
        const int endSlot = anchorSlots[span + 1];
        const int startTime = anchorTimes[span];
        const int endTime = anchorTimes[span + 1];
        const int slotCount = endSlot - startSlot;
        if (slotCount <= 0) {
            t.mclkOffset[startSlot] = startTime;
            continue;
        }
        for (int slot = startSlot; slot <= endSlot; slot++) {
            const int numer = (endTime - startTime) * (slot - startSlot);
            t.mclkOffset[slot] = startTime + (numer / slotCount);
        }
    }
}

inline constexpr void precomputeNextExternal(VDPSlotTable& t) {
    t.nextExternalSlot[t.count] = t.count;
    for (int i = t.count - 1; i >= 0; i--) {
        if (t.types[i] == SLOT_EXTERNAL) {
            t.nextExternalSlot[i] = i;
        } else {
            t.nextExternalSlot[i] = t.nextExternalSlot[i + 1];
        }
    }
    t.externalSlotCount = 0;
    for (int i = 0; i < t.count; i++) {
        if (t.types[i] == SLOT_EXTERNAL) {
            t.externalSlotCount++;
        }
    }
}

// Precompute next "action slot" — slots that need processing in the
// slot-driven loop (EXTERNAL, VSCROLL_LATCH, NT_A per-column vscroll
// capture, or PAT_B1 for column output).
inline constexpr void precomputeNextActionSlot(VDPSlotTable& t) {
    t.nextActionSlot[t.count] = t.count;
    for (int i = t.count - 1; i >= 0; i--) {
        VDPSlotType st = t.info[i].type;
        if (st == SLOT_EXTERNAL || st == SLOT_VSCROLL_LATCH ||
            st == SLOT_PAT_B1 || st == SLOT_NT_A) {
            t.nextActionSlot[i] = i;
        } else {
            t.nextActionSlot[i] = t.nextActionSlot[i + 1];
        }
    }
}

inline constexpr void computeM68kReverseLookup(VDPSlotTable& t) {
    // Map each M68K cycle (0..487) to the slot index active at that cycle.
    // mclkOffset converts to master clocks; M68K cycle i corresponds to
    // master clock range [i*7, (i+1)*7).
    for (int cyc = 0; cyc < VDP_MAX_M68K_CYCLES; cyc++) {
        int mclk = cyc * 7;
        // Find the last slot whose mclkOffset <= mclk
        int slot = 0;
        for (int s = 0; s < t.count; s++) {
            if (t.mclkOffset[s] <= mclk) {
                slot = s;
            } else {
                break;
            }
        }
        t.slotAtM68kCycle[cyc] = slot;
    }

    // Precompute the first M68K cycle at which the slot index reaches the
    // H-int assert edge and the HBlank-start edge.
    t.hintAssertM68kCycle = VDP_MAX_M68K_CYCLES - 1;
    t.hblankStartM68kCycle = VDP_MAX_M68K_CYCLES - 1;
    for (int cyc = 0; cyc < VDP_MAX_M68K_CYCLES; cyc++) {
        if (t.slotAtM68kCycle[cyc] >= t.hintAssertSlot) {
            t.hintAssertM68kCycle = cyc;
            break;
        }
    }
    for (int cyc = 0; cyc < VDP_MAX_M68K_CYCLES; cyc++) {
        if (t.slotAtM68kCycle[cyc] >= t.hblankStartSlot) {
            t.hblankStartM68kCycle = cyc;
            break;
        }
    }
}

inline constexpr void computePixelsAndHCounter(VDPSlotTable& t) {
    const int aw = t.activeWidth;
    // Pixel output: slots before hblankEndSlot and >= hblankStartSlot are blanking.
    // Active slots map linearly to pixels 0..activeWidth-1.
    int activeSlots = t.hblankStartSlot - t.hblankEndSlot;
    if (activeSlots <= 0) activeSlots = 1;

    for (int s = 0; s < t.count; s++) {
        if (s < t.hblankEndSlot || s >= t.hblankStartSlot) {
            t.pixelAtSlot[s] = aw; // blanking
        } else {
            int pos = ((s - t.hblankEndSlot) * aw) / activeSlots;
            if (pos < 0) pos = 0;
            if (pos > aw) pos = aw;
            t.pixelAtSlot[s] = pos;
        }
    }

    // H-counter: follows the real hardware's discontinuous counting pattern.
    // The 9-bit dot counter has a jump during HSync, producing a gap in the
    // 8-bit H counter value returned to the CPU (upper 8 bits of dot counter).
    //
    // H40 (210 slots): 0x00-0xB6, jump to 0xE4, 0xE4-0xFF (211 unique values,
    //   but 0xB6 and 0xE4 each last only 1 dot vs 2 for normal values, so they
    //   share one slot — we skip 0xB6). Line starts at V-counter increment = 0xA5.
    //
    // H32 (171 slots): 0x00-0x93, jump to 0xE9, 0xE9-0xFF (171 unique values,
    //   exact 1:1 mapping). Line starts at V-counter increment = 0x85.
    if (aw == 320) {
        // H40: slots 0-16 = 0xA5..0xB5, slot 17+ = 0xE4..0xFF,0x00..0xA4
        for (int s = 0; s < t.count; s++) {
            if (s < 17) {
                t.hcounterAtSlot[s] = 0xA5 + s;
            } else {
                t.hcounterAtSlot[s] = (0xE4 + (s - 17)) & 0xFF;
            }
        }
    } else {
        // H32: slots 0-14 = 0x85..0x93, slot 15+ = 0xE9..0xFF,0x00..0x84
        for (int s = 0; s < t.count; s++) {
            if (s < 15) {
                t.hcounterAtSlot[s] = 0x85 + s;
            } else {
                t.hcounterAtSlot[s] = (0xE9 + (s - 15)) & 0xFF;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// H40 active line (320px mode, display enabled, not in VBlank)
// ---------------------------------------------------------------------------
// 210 slots total, 18 external slots per line.
// Pattern fetches dominate during active display; external slots are
// interspersed roughly every 10 pattern slots, plus one in early HBlank.
//
// HBlank boundaries derived from the old lineCycles thresholds:
//   HBlank start: lineCycles >= 414  → mclk 2898 → slot 178
//   HBlank end:   lineCycles < 14    → mclk 98   → slot 6
//
// 18 external slots match 68K→CRAM bandwidth (18 words/line) and
// 68K→VRAM (18 bytes / 2 = 9 words at 2 slots each). Fill DMA yields
// 18 bytes/line vs the manual's 17 — a tolerable 1-byte overshoot.
inline constexpr VDPSlotTable buildH40Active() {
    VDPSlotTable t{};
    t.count = 210;
    t.activeWidth = 320;
    // H-int counter/IRQ advances at the V-counter line boundary, after the
    // line-change phase that corresponds to slot 0 in this table.
    t.hintAssertSlot = 0;
    t.hblankEndSlot = 6;      // Active display starts at slot 6
    t.hblankStartSlot = 178;  // HBlank starts at slot 178

    // Default all to pattern
    for (int i = 0; i < t.count; i++) {
        t.types[i] = SLOT_PATTERN;
    }

    // 17 external slots in active display + 1 in early HBlank = 18 total
    const int extSlots[] = {
        15, 25, 35, 45, 55, 65, 75, 85,
        95, 105, 115, 125, 135, 145, 155, 165, 171,
        178  // one external window in early HBlank
    };
    for (int s : extSlots) {
        t.types[s] = SLOT_EXTERNAL;
    }

    // Refresh slots (5 per line in H40, at fixed positions within HBlank)
    const int refreshSlots[] = {182, 190, 198, 204, 209};
    for (int s : refreshSlots) {
        t.types[s] = SLOT_REFRESH;
    }

    // No H40 line-wide VSCROLL_LATCH: the renderer reads per-column
    // columnVscroll[col][layer] captured at each column's SLOT_NT_A, which
    // is the hardware-correct source (each NT fetch reads VSRAM at its own
    // slot -- there is no single latch cycle on real hardware). H32 still
    // uses a line-wide latch (see buildH32Active).

    // Anchor the active-line external access windows to verified H40 FIFO
    // timings, then interpolate pattern slots between them. Formula One /
    // Kawasaki rely on these non-uniform windows.
    const int extTimes[] = {
        352, 820, 948, 1076, 1332, 1460, 1588, 1844,
        1972, 2100, 2356, 2484, 2612, 2868, 2996, 3124, 3364, 3380
    };
    applyAnchoredTimings(t, extSlots, extTimes);

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);

    // Populate info[] with slot sub-types derived from hardware analysis.
    // Column blocks surround each active-display external slot: NT_A at E-1,
    // EXT at E, PAT_A0..PAT_B1 at E+1..E+6. Gap slots are SAT_SCAN.

    // Default all to mirror the types[] array
    for (int i = 0; i < t.count; i++) {
        t.info[i] = {t.types[i], 0xFF};
    }

    // Column blocks around the 17 active-display external slots
    const int activeExtSlots[] = {15, 25, 35, 45, 55, 65, 75, 85, 95, 105, 115, 125, 135, 145, 155, 165, 171};
    for (int blk = 0; blk < 17; blk++) {
        int E = activeExtSlots[blk];
        uint8_t col = static_cast<uint8_t>(blk);

        if (E - 1 >= 0)          t.info[E - 1] = {SLOT_NT_A,    col};
        t.info[E]                 = {SLOT_EXTERNAL, col};
        if (E + 1 < t.count)     t.info[E + 1] = {SLOT_PAT_A0,  col};
        if (E + 2 < t.count)     t.info[E + 2] = {SLOT_PAT_A1,  col};
        if (E + 3 < t.count)     t.info[E + 3] = {SLOT_NT_B,    col};
        if (E + 4 < t.count)     t.info[E + 4] = {SLOT_SPR_MAP, col};
        if (E + 5 < t.count)     t.info[E + 5] = {SLOT_PAT_B0,  col};
        if (E + 6 < t.count)     t.info[E + 6] = {SLOT_PAT_B1,  col};
    }

    // Gap slots between column blocks (active display): SAT_SCAN
    for (int i = 6; i < 178; i++) {
        if (t.info[i].type == SLOT_PATTERN) {
            t.info[i] = {SLOT_SAT_SCAN, 0xFF};
        }
    }

    // Early H-blank (slots 0-5): sprite/hscroll operations
    for (int i = 0; i < 6; i++) {
        if (t.types[i] == SLOT_PATTERN) {
            t.info[i] = {SLOT_SAT_SCAN, 0xFF};
        } else if (t.types[i] == SLOT_VSCROLL_LATCH) {
            t.info[i] = {SLOT_VSCROLL_LATCH, 0xFF};
        }
    }

    // Late H-blank (slots 178+): SPR_PATTERN and refresh
    for (int i = 178; i < t.count; i++) {
        if (t.types[i] == SLOT_EXTERNAL) {
            t.info[i] = {SLOT_EXTERNAL, 0xFF};
        } else if (t.types[i] == SLOT_REFRESH) {
            t.info[i] = {SLOT_REFRESH, 0xFF};
        } else {
            t.info[i] = {SLOT_SPR_PATTERN, 0xFF};
        }
    }

    precomputeNextActionSlot(t);
    return t;
}

// ---------------------------------------------------------------------------
// H40 blank line (320px mode, VBlank or display disabled)
// ---------------------------------------------------------------------------
// All non-refresh slots become external (no pattern fetches needed).
// ~204 external slots.
inline constexpr VDPSlotTable buildH40Blank() {
    VDPSlotTable t{};
    t.count = 210;
    t.activeWidth = 320;
    t.hintAssertSlot = t.count - 1;
    t.hblankEndSlot = 0;
    t.hblankStartSlot = 210;  // Entire line is "blank" — no active display

    // Default all to external
    for (int i = 0; i < t.count; i++) {
        t.types[i] = SLOT_EXTERNAL;
    }

    // Refresh slots still occur at fixed positions (5 per line + 1 extra)
    const int refreshSlots[] = {25, 57, 89, 121, 153, 185};
    for (int s : refreshSlots) {
        t.types[s] = SLOT_REFRESH;
    }

    for (int i = 0; i < t.count; i++) {
        t.mclkOffset[i] = (i * 3420) / t.count;
    }

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);

    for (int i = 0; i < t.count; i++) {
        t.info[i] = {t.types[i], 0xFF};
    }

    precomputeNextActionSlot(t);
    return t;
}

// ---------------------------------------------------------------------------
// H32 active line (256px mode, display enabled, not in VBlank)
// ---------------------------------------------------------------------------
// 171 slots total, 16 external slots per line.
//
// HBlank boundaries from old lineCycles thresholds:
//   HBlank start: lineCycles >= 332  → mclk 2324 → slot 117
//   HBlank end:   lineCycles < 11    → mclk 77   → slot 4
//
// 16 external slots match 68K→CRAM (16 words/line) and 68K→VRAM
// (16 bytes / 2 = 8 words at 2 slots each).
inline constexpr VDPSlotTable buildH32Active() {
    VDPSlotTable t{};
    t.count = 171;
    t.activeWidth = 256;
    // H-int counter/IRQ advances at the V-counter line boundary in H32 too.
    // F1 depends on the handler reaching its display-disable write while the
    // target scanline is still current.
    t.hintAssertSlot = 0;
    t.hblankEndSlot = 4;
    t.hblankStartSlot = 117;

    for (int i = 0; i < t.count; i++) {
        t.types[i] = SLOT_PATTERN;
    }

    // 12 external slots in active display + 4 in HBlank = 16 total
    const int extSlots[] = {
        12, 22, 31, 40, 50, 59, 68, 78,
        87, 96, 106, 115,
        118, 124, 134, 139  // HBlank external windows
    };
    for (int s : extSlots) {
        t.types[s] = SLOT_EXTERNAL;
    }

    // Refresh slots (4 per line in H32)
    const int refreshSlots[] = {144, 153, 162, 170};
    for (int s : refreshSlots) {
        t.types[s] = SLOT_REFRESH;
    }

    // H32 full-screen vscroll is sampled at line start after previous-line
    // carry-over VSRAM drains, so there is no active-display latch slot here.

    // Anchor the active-line external access windows to verified H32 FIFO
    // timings. Formula One is one of the games that flickers when these
    // windows are too evenly spaced.
    const int extTimes[] = {
        230, 510, 810, 970, 1130, 1450, 1610, 1770,
        2090, 2250, 2410, 2730, 2890, 3050, 3350, 3370
    };
    applyAnchoredTimings(t, extSlots, extTimes);

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);

    // Populate info[] with slot sub-types derived from hardware analysis.
    // Column blocks surround each active-display external slot: NT_A at E-1,
    // EXT at E, PAT_A0..PAT_B1 at E+1..E+6. Gap slots are SAT_SCAN.

    // Default all to mirror the types[] array
    for (int i = 0; i < t.count; i++) {
        t.info[i] = {t.types[i], 0xFF};
    }

    // Column blocks around the 12 active-display external slots
    const int activeH32ExtSlots[] = {12, 22, 31, 40, 50, 59, 68, 78, 87, 96, 106, 115};
    for (int blk = 0; blk < 12; blk++) {
        int E = activeH32ExtSlots[blk];
        uint8_t col = static_cast<uint8_t>(blk);

        if (E - 1 >= 0)          t.info[E - 1] = {SLOT_NT_A,    col};
        t.info[E]                 = {SLOT_EXTERNAL, col};
        if (E + 1 < t.count)     t.info[E + 1] = {SLOT_PAT_A0,  col};
        if (E + 2 < t.count)     t.info[E + 2] = {SLOT_PAT_A1,  col};
        if (E + 3 < t.count)     t.info[E + 3] = {SLOT_NT_B,    col};
        if (E + 4 < t.count)     t.info[E + 4] = {SLOT_SPR_MAP, col};
        if (E + 5 < t.count)     t.info[E + 5] = {SLOT_PAT_B0,  col};
        if (E + 6 < t.count)     t.info[E + 6] = {SLOT_PAT_B1,  col};
    }

    // Gap slots between column blocks (active display): SAT_SCAN
    for (int i = 4; i < 117; i++) {
        if (t.info[i].type == SLOT_PATTERN) {
            t.info[i] = {SLOT_SAT_SCAN, 0xFF};
        }
    }

    // Early H-blank (slots 0-3)
    for (int i = 0; i < 4; i++) {
        if (t.types[i] == SLOT_VSCROLL_LATCH) {
            t.info[i] = {SLOT_VSCROLL_LATCH, 0xFF};
        } else if (t.types[i] == SLOT_PATTERN) {
            t.info[i] = {SLOT_SAT_SCAN, 0xFF};
        }
    }

    // Late H-blank (slots 117+): SPR_PATTERN, refresh, and external
    for (int i = 117; i < t.count; i++) {
        if (t.types[i] == SLOT_EXTERNAL) {
            t.info[i] = {SLOT_EXTERNAL, 0xFF};
        } else if (t.types[i] == SLOT_REFRESH) {
            t.info[i] = {SLOT_REFRESH, 0xFF};
        } else if (t.types[i] == SLOT_VSCROLL_LATCH) {
            t.info[i] = {SLOT_VSCROLL_LATCH, 0xFF};
        } else {
            t.info[i] = {SLOT_SPR_PATTERN, 0xFF};
        }
    }

    // Preserve behavior-bearing type overrides after column metadata is
    // populated. Slot-driven dispatch uses info[].type, so it must agree with
    // types[] for synthetic events like H32's full-screen VSRAM latch.
    for (int i = 0; i < t.count; i++) {
        if (t.types[i] == SLOT_VSCROLL_LATCH) {
            t.info[i] = {SLOT_VSCROLL_LATCH, 0xFF};
        }
    }

    precomputeNextActionSlot(t);
    return t;
}

// ---------------------------------------------------------------------------
// H32 blank line (256px mode, VBlank or display disabled)
// ---------------------------------------------------------------------------
// ~166 external slots.
inline constexpr VDPSlotTable buildH32Blank() {
    VDPSlotTable t{};
    t.count = 171;
    t.activeWidth = 256;
    t.hintAssertSlot = t.count - 1;
    t.hblankEndSlot = 0;
    t.hblankStartSlot = 171;

    for (int i = 0; i < t.count; i++) {
        t.types[i] = SLOT_EXTERNAL;
    }

    // Refresh slots (5 per line during blank)
    const int refreshSlots[] = {20, 54, 88, 122, 156};
    for (int s : refreshSlots) {
        t.types[s] = SLOT_REFRESH;
    }

    for (int i = 0; i < t.count; i++) {
        t.mclkOffset[i] = (i * 3420) / t.count;
    }

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);

    for (int i = 0; i < t.count; i++) {
        t.info[i] = {t.types[i], 0xFF};
    }

    precomputeNextActionSlot(t);
    return t;
}

} // namespace vdp_slot_detail

// Four global slot tables, one per mode combination.
static const VDPSlotTable kH40Active = vdp_slot_detail::buildH40Active();
static const VDPSlotTable kH40Blank  = vdp_slot_detail::buildH40Blank();
static const VDPSlotTable kH32Active = vdp_slot_detail::buildH32Active();
static const VDPSlotTable kH32Blank  = vdp_slot_detail::buildH32Blank();
