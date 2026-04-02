// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/psg.h"
#include <cstring>

// Volume table: 2dB steps, 0=max, 15=off
const s16 PSG::volumeTable[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031, 819, 650, 516, 410, 326, 0
};

PSG::PSG() {
    reset();
}

void PSG::reset() {
    std::memset(tone, 0, sizeof(tone));
    std::memset(&noise, 0, sizeof(noise));
    
    for (int i = 0; i < 3; i++) {
        tone[i].volume = 15; // Off
        tone[i].output = -1; // Match hardware power-on polarity (verified on 315-5313A)
    }
    noise.volume = 15;
    noise.output = -1;
    
    lfsr = 0x8000;
    latchedChannel = 0;
    latchedVolume = false;
    clockCounter = 0;
    sampleAccum = 0;
    sampleCount = 0;
}

void PSG::write(u8 data) {
    if (data & 0x80) {
        // Latch byte
        latchedChannel = (data >> 5) & 3;
        latchedVolume = (data >> 4) & 1;
        
        if (latchedChannel < 3) {
            // Tone channel
            if (latchedVolume) {
                tone[latchedChannel].volume = data & 0x0F;
            } else {
                tone[latchedChannel].tone = (tone[latchedChannel].tone & 0x3F0) | (data & 0x0F);
            }
        } else {
            // Noise channel
            if (latchedVolume) {
                noise.volume = data & 0x0F;
            } else {
                noise.tone = data & 0x07;
                lfsr = 0x8000; // Reset LFSR on noise register write
                // Sync noise counter to tone[2] when mode 3 selected (hardware behavior)
                if ((noise.tone & 3) == 3) {
                    noise.counter = tone[2].counter;
                }
            }
        }
    } else {
        // Data byte — update whichever register is latched
        if (latchedChannel < 3) {
            if (latchedVolume) {
                tone[latchedChannel].volume = data & 0x0F;
            } else {
                tone[latchedChannel].tone = (tone[latchedChannel].tone & 0x00F) | ((data & 0x3F) << 4);
            }
        } else {
            if (latchedVolume) {
                noise.volume = data & 0x0F;
            } else {
                // The noise register is only 3 bits wide, but data bytes still
                // update those low bits on Sega PSG variants. Treat this as a
                // noise-register write, including the LFSR reset.
                noise.tone = data & 0x07;
                lfsr = 0x8000;
                if ((noise.tone & 3) == 3) {
                    noise.counter = tone[2].counter;
                }
            }
        }
    }

}

void PSG::clock(int m68kCycles) {
    // PSG ticks at Z80 clock / 16 = master / (15*16) = master / 240
    // Input is M68K cycles (master / 7), so multiply by 7 to get master cycles
    clockCounter += m68kCycles * 7;

    while (clockCounter >= 240) {
        clockCounter -= 240;
        
        // Update tone channels
        // Period 0 acts as period 1 on Sega variant (highest frequency / DC)
        // This is the basis for PSG PCM sample playback used by some games
        for (int i = 0; i < 3; i++) {
            tone[i].counter--;
            if (tone[i].counter <= 0) {
                tone[i].counter = (tone[i].tone == 0) ? 1 : tone[i].tone;
                tone[i].output = -tone[i].output;
            }
        }
        
        // Update noise channel
        int noisePeriod;
        switch (noise.tone & 3) {
            case 0: noisePeriod = 0x10; break;
            case 1: noisePeriod = 0x20; break;
            case 2: noisePeriod = 0x40; break;
            case 3: noisePeriod = tone[2].tone; break; // Use tone 2 frequency
            default: noisePeriod = 0x10; break;
        }
        
        noise.counter--;
        if (noise.counter <= 0) {
            noise.counter = (noisePeriod == 0) ? 1 : noisePeriod;
            noise.output = -noise.output;

            // Hardware shifts LFSR only on rising edge (output transitions to +1)
            // This halves the effective shift rate compared to shifting every reload
            if (noise.output > 0) {
                if (noise.tone & 4) {
                    // White noise: Fibonacci LFSR with taps at bits 0 and 3 (Sega variant)
                    bool feedback = ((lfsr >> 0) ^ (lfsr >> 3)) & 1;
                    lfsr = (lfsr >> 1) | (feedback << 15);
                } else {
                    // Periodic noise: shift right, feeding bit 0 back to bit 15
                    bool bit = lfsr & 1;
                    lfsr = (lfsr >> 1) | (bit << 15);
                }
            }
        }

        // Accumulate mixed sample for averaging (anti-aliasing)
        s32 mix = 0;
        for (int i = 0; i < 3; i++) {
            s16 vol = volumeTable[tone[i].volume];
            mix += (tone[i].output > 0) ? vol : -vol;
        }
        s16 noiseVol = volumeTable[noise.volume];
        mix += (lfsr & 1) ? noiseVol : -noiseVol;
        sampleAccum += mix;
        sampleCount++;
    }
}

s16 PSG::getSample() {
    s32 sample;

    if (sampleCount > 0) {
        // Return averaged accumulator (anti-aliased)
        sample = sampleAccum / sampleCount;
        sampleAccum = 0;
        sampleCount = 0;
    } else {
        // Fallback: compute single point sample
        sample = 0;
        for (int i = 0; i < 3; i++) {
            s16 vol = volumeTable[tone[i].volume];
            sample += (tone[i].output > 0) ? vol : -vol;
        }
        s16 noiseVol = volumeTable[noise.volume];
        sample += (lfsr & 1) ? noiseVol : -noiseVol;
    }

    // Clamp to s16 range (raw sum of 4 channels can exceed ±32767)
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    return static_cast<s16>(sample);
}
