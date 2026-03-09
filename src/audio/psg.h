// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "types.h"

struct PSGChannel {
    u16 tone;           // Tone register (10-bit for tone, 3-bit for noise)
    u8 volume;          // Volume (4-bit, 0=max, 15=off)
    int counter;        // Frequency counter
    int output;         // Current output state (+1 or -1)
};

class PSG {
public:
    PSG();
    void reset();
    void write(u8 data);
    void clock(int cycles);
    s16 getSample();

    friend class Genesis;
    friend class PSGTest;

private:
    PSGChannel tone[3];     // 3 tone channels
    PSGChannel noise;       // 1 noise channel
    u16 lfsr;               // Linear feedback shift register for noise
    u8 latchedChannel;      // Currently latched channel
    bool latchedVolume;     // Latched for volume or tone
    
    int clockCounter;

    s32 sampleAccum;
    int sampleCount;

    static const s16 volumeTable[16];
};
