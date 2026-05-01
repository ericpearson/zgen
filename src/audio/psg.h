// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include "types.h"

// Forward declare blip_t (defined in blip_buf.h)
struct blip_t;

struct PSGChannel {
    u16 tone;           // Tone register (10-bit for tone, 3-bit for noise)
    u8 volume;          // Volume (4-bit, 0=max, 15=off)
    int counter;        // Frequency counter
    int output;         // Current output state (+1 or -1)
    int lastAmp;        // Last amplitude sent to blip_buf (for delta calculation)
};

class PSG {
public:
    PSG();
    ~PSG();
    void reset();
    void write(u8 data);
    void clock(int cycles);
    s16 getSample();

    // blip_buf band-limited synthesis
    bool initBlip(double clockRate, double sampleRate);
    void shutdownBlip();
    void endFrame(int clockDuration);
    int samplesAvailable() const;
    int readSamples(short* out, int count);

    friend class Genesis;
    friend class PSGTest;

private:
    PSGChannel tone[3];     // 3 tone channels
    PSGChannel noise;       // 1 noise channel
    u16 lfsr;               // Linear feedback shift register for noise
    u8 latchedChannel;      // Currently latched channel
    bool latchedVolume;     // Latched for volume or tone

    int clockCounter;
    int blipTime;           // Current time in blip_buf clocks

    s32 sampleAccum;
    int sampleCount;

    // blip_buf buffers (one per channel for proper mixing)
    blip_t* blipBuf[4];     // 3 tone + 1 noise
    bool blipEnabled;

    void updateBlipAmplitude(int channel, int newAmp);

    static const s16 volumeTable[16];
};
