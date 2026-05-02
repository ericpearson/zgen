// Copyright (C) 2026 pagefault
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/psg.h"
#include "blip_buf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Volume table: 2dB steps, 0=max, 15=off
const s16 PSG::volumeTable[16] = {
    8191, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
    1298, 1031, 819, 650, 516, 410, 326, 0
};

PSG::PSG() : blipEnabled(false), blipTime(0) {
    for (int i = 0; i < 4; i++) {
        blipBuf[i] = nullptr;
    }
    reset();
}

PSG::~PSG() {
    shutdownBlip();
}

void PSG::reset() {
    std::memset(tone, 0, sizeof(tone));
    std::memset(&noise, 0, sizeof(noise));

    for (int i = 0; i < 3; i++) {
        tone[i].volume = 15; // Off
        tone[i].output = -1; // Match hardware power-on polarity (verified on 315-5313A)
        tone[i].lastAmp = 0;
    }
    noise.volume = 15;
    noise.output = -1;
    noise.lastAmp = 0;

    lfsr = 0x8000;
    latchedChannel = 0;
    latchedVolume = false;
    clockCounter = 0;
    blipTime = 0;
    sampleAccum = 0;
    sampleCount = 0;

    // Clear blip buffers if active
    for (int i = 0; i < 4; i++) {
        if (blipBuf[i]) {
            blip_clear(blipBuf[i]);
        }
    }
}

void PSG::write(u8 data) {
    static bool logEnabled = false;
    static bool logChecked = false;
    if (!logChecked) {
        const char* e = std::getenv("GENESIS_LOG_PSG_WRITES");
        logEnabled = e && e[0] != '0';
        logChecked = true;
    }
    if (logEnabled) {
        std::fprintf(stderr, "[PSG] %02X\n", data);
    }
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

void PSG::clockMaster(int masterCycles) {
    if (masterCycles <= 0) {
        return;
    }

    // PSG ticks at Z80 clock / 16 = master / (15*16) = master / 240
    clockCounter += masterCycles;

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

                // Update blip_buf with new amplitude on transition
                if (blipEnabled) {
                    s16 vol = volumeTable[tone[i].volume];
                    int newAmp = (tone[i].output > 0) ? vol : 0;
                    updateBlipAmplitude(i, newAmp);
                }
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

            // Update blip_buf with new amplitude (noise channel = index 3)
            if (blipEnabled) {
                s16 noiseVol = volumeTable[noise.volume];
                int newAmp = (lfsr & 1) ? noiseVol : 0;
                updateBlipAmplitude(3, newAmp);
            }
        }

        // Advance blip time (1 PSG clock tick)
        if (blipEnabled) {
            blipTime++;
        }

        // Accumulate mixed sample for averaging (anti-aliasing) - legacy path
        // Hardware PSG uses unipolar output: 0 when low, +vol when high
        // (NOT bipolar -vol/+vol like older emulators assumed)
        s32 mix = 0;
        for (int i = 0; i < 3; i++) {
            s16 vol = volumeTable[tone[i].volume];
            mix += (tone[i].output > 0) ? vol : 0;
        }
        s16 noiseVol = volumeTable[noise.volume];
        mix += (lfsr & 1) ? noiseVol : 0;
        sampleAccum += mix;
        sampleCount++;
    }
}

void PSG::clock(int m68kCycles) {
    clockMaster(m68kCycles * 7);
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
        // Hardware PSG uses unipolar output: 0 when low, +vol when high
        sample = 0;
        for (int i = 0; i < 3; i++) {
            s16 vol = volumeTable[tone[i].volume];
            sample += (tone[i].output > 0) ? vol : 0;
        }
        s16 noiseVol = volumeTable[noise.volume];
        sample += (lfsr & 1) ? noiseVol : 0;
    }

    // Clamp to s16 range (raw sum of 4 channels can exceed ±32767)
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    return static_cast<s16>(sample);
}

// =====================================================================
// blip_buf band-limited synthesis
// =====================================================================

bool PSG::initBlip(double clockRate, double sampleRate) {
    shutdownBlip();  // Clean up any existing buffers

    // Create buffers - each can hold ~100ms of samples
    int bufferSize = static_cast<int>(sampleRate / 10);

    for (int i = 0; i < 4; i++) {
        blipBuf[i] = blip_new(bufferSize);
        if (!blipBuf[i]) {
            shutdownBlip();
            return false;
        }
        blip_set_rates(blipBuf[i], clockRate, sampleRate);
    }

    blipEnabled = true;
    blipTime = 0;
    return true;
}

void PSG::shutdownBlip() {
    for (int i = 0; i < 4; i++) {
        if (blipBuf[i]) {
            blip_delete(blipBuf[i]);
            blipBuf[i] = nullptr;
        }
    }
    blipEnabled = false;
}

void PSG::updateBlipAmplitude(int channel, int newAmp) {
    if (!blipEnabled || !blipBuf[channel]) return;

    PSGChannel* ch = (channel < 3) ? &tone[channel] : &noise;
    int delta = newAmp - ch->lastAmp;
    if (delta != 0) {
        blip_add_delta(blipBuf[channel], blipTime, delta);
        ch->lastAmp = newAmp;
    }
}

void PSG::endFrame(int clockDuration) {
    if (!blipEnabled) return;

    for (int i = 0; i < 4; i++) {
        if (blipBuf[i]) {
            blip_end_frame(blipBuf[i], clockDuration);
        }
    }
    blipTime = 0;  // Reset time for next frame
}

int PSG::samplesAvailable() const {
    if (!blipEnabled || !blipBuf[0]) return 0;
    return blip_samples_avail(blipBuf[0]);
}

int PSG::readSamples(short* out, int count) {
    if (!blipEnabled || !blipBuf[0]) return 0;

    // Read from first buffer to get count
    int available = blip_samples_avail(blipBuf[0]);
    if (count > available) count = available;
    if (count <= 0) return 0;

    // Temporary buffers for each channel
    short temp[4][4096];
    int toRead = (count > 4096) ? 4096 : count;

    for (int i = 0; i < 4; i++) {
        if (blipBuf[i]) {
            blip_read_samples(blipBuf[i], temp[i], toRead, 0);
        } else {
            std::memset(temp[i], 0, toRead * sizeof(short));
        }
    }

    // Mix channels
    for (int s = 0; s < toRead; s++) {
        int mix = temp[0][s] + temp[1][s] + temp[2][s] + temp[3][s];
        // Clamp to s16 range
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        out[s] = static_cast<short>(mix);
    }

    return toRead;
}
