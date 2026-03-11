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
    SLOT_PATTERN
};

// Maximum slot count across all modes (H40 blank has the most).
static constexpr int VDP_MAX_SLOTS = 210;
static constexpr int VDP_MAX_M68K_CYCLES = 488;

struct VDPSlotTable {
    int count;                              // Total slots this scanline
    VDPSlotType types[VDP_MAX_SLOTS];       // Slot type at each position
    int mclkOffset[VDP_MAX_SLOTS];          // Master-clock offset (0..3419)
    int hblankStartSlot;                    // First slot in HBlank
    int hblankEndSlot;                      // First slot after HBlank (start of active)
    int nextExternalSlot[VDP_MAX_SLOTS + 1]; // For slot i, index of next external slot (inclusive); count if none
    int externalSlotCount;                  // Total external slots
    int pixelAtSlot[VDP_MAX_SLOTS];         // Visible pixel output position (activeWidth if blanking)
    int hcounterAtSlot[VDP_MAX_SLOTS];      // H-counter value at each slot
    int slotAtM68kCycle[VDP_MAX_M68K_CYCLES]; // Reverse lookup: M68K cycle -> slot index
    int hblankStartM68kCycle;               // First M68K cycle where slot >= hblankStartSlot
    int activeWidth;                        // 320 or 256
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

    // Precompute the first M68K cycle at which the slot index reaches hblankStartSlot.
    // This replaces a runtime mclkOffset[hblankStartSlot] / 7 that truncates.
    t.hblankStartM68kCycle = 0;
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

    // Master clock offsets: 3420 mclk per line, 210 slots = ~16.28 mclk/slot
    for (int i = 0; i < t.count; i++) {
        t.mclkOffset[i] = (i * 3420) / t.count;
    }

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);
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

    for (int i = 0; i < t.count; i++) {
        t.mclkOffset[i] = (i * 3420) / t.count;
    }

    precomputeNextExternal(t);
    computePixelsAndHCounter(t);
    computeM68kReverseLookup(t);
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
    return t;
}

} // namespace vdp_slot_detail

// Four global slot tables, one per mode combination.
static const VDPSlotTable kH40Active = vdp_slot_detail::buildH40Active();
static const VDPSlotTable kH40Blank  = vdp_slot_detail::buildH40Blank();
static const VDPSlotTable kH32Active = vdp_slot_detail::buildH32Active();
static const VDPSlotTable kH32Blank  = vdp_slot_detail::buildH32Blank();
